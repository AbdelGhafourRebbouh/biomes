#include <windows.h>
#include <unknwn.h>
#include <objbase.h>
#include <winerror.h>
#include <windowsx.h>
#include <dwmapi.h>

#include <iostream>
#include <string>
#include <functional>
#include <deque>

#include <WebView2.h>
#include "../../include/ui/webview_window.hpp"
#include "../../include/core/app_paths.hpp"
#include "../../resources/resource.h"

namespace {
constexpr UINT kDispatchWebMessage = WM_APP + 71;
constexpr UINT kPageReady = WM_APP + 72;
std::function<void()> pageReadyCallback;
bool pageReady = false;
EventRegistrationToken messageToken{}, navigationToken{}, completedToken{};
HWND lastAppWindow = nullptr;
DWORD lastAppPid = 0;
unsigned long long hostGeneration = 0;
HMODULE loaderModule = nullptr;

void RememberAppWindow(HWND hwnd) {
    DWORD pid = 0;
    if (hwnd && IsWindow(hwnd)) GetWindowThreadProcessId(hwnd, &pid);
    if (pid && pid != GetCurrentProcessId()) { lastAppWindow = hwnd; lastAppPid = pid; }
}

std::deque<std::string> pendingWebMessages;
bool dispatchingWebMessage = false;
std::function<void()> showRequested, closeRequested;
bool shuttingDown = false;
std::wstring trustedPage;
}

// ============================================================================
// Function Pointer Definition for Dynamic Loading in MinGW
// ============================================================================

typedef HRESULT (__stdcall *PFN_CreateCoreWebView2EnvironmentWithOptions)(
    PCWSTR browserExecutableFolder,
    PCWSTR userDataFolder,
    ICoreWebView2EnvironmentOptions* environmentOptions,
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler* environmentCreatedHandler
);

// Minimal COM callbacks with correct interface identity and owned references.
template <typename Interface, typename... Args>
class ComCallbackImpl final : public Interface {
    std::function<HRESULT(Args...)> callback_;
    long references_ = 1;
public:
    explicit ComCallbackImpl(std::function<HRESULT(Args...)> callback) : callback_(std::move(callback)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (id != __uuidof(IUnknown) && id != __uuidof(Interface)) return E_NOINTERFACE;
        *result = static_cast<Interface*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&references_); }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto remaining = InterlockedDecrement(&references_);
        if (!remaining) delete this;
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE Invoke(Args... args) override {
        try { return callback_(args...); } catch (...) { return E_FAIL; }
    }
};

template <typename Interface, typename... Args, typename F>
Interface* CreateCallbackRaw(F&& callback) {
    return new ComCallbackImpl<Interface, Args...>(std::forward<F>(callback));
}

// ============================================================================
// Static Member Definitions
// ============================================================================

HWND WebViewWindow::s_hwnd = nullptr;
ICoreWebView2Controller* WebViewWindow::s_controller = nullptr;
ICoreWebView2* WebViewWindow::s_webview = nullptr;
std::function<void(const std::string&)> WebViewWindow::s_onMessageReceived = nullptr;
std::function<void(int)> WebViewWindow::s_onHotkeyPressed = nullptr;
std::function<void()> WebViewWindow::s_onDisplayChanged = nullptr;

bool WebViewWindow::Initialize(HINSTANCE hInstance, int nCmdShow, const std::string& startUrl) {
    if (s_hwnd) return true;
    if (shuttingDown) return false;
    ++hostGeneration;
    RememberAppWindow(GetForegroundWindow());
    const int urlLength = MultiByteToWideChar(CP_UTF8, 0, startUrl.data(), static_cast<int>(startUrl.size()), nullptr, 0);
    trustedPage.resize(urlLength);
    MultiByteToWideChar(CP_UTF8, 0, startUrl.data(), static_cast<int>(startUrl.size()), trustedPage.data(), urlLength);
    WNDCLASSEXA wc = { sizeof(WNDCLASSEXA) };
    wc.lpfnWndProc   = WindowProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = "BiomesWebViewWindowClass";
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    // Shared resource icons belong to the module; do not DestroyIcon these.
    wc.hIcon = static_cast<HICON>(LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_BIOMES), IMAGE_ICON,
        GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_SHARED));
    wc.hIconSm = static_cast<HICON>(LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_BIOMES), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));

    RegisterClassExA(&wc);

    s_hwnd = CreateWindowExA(
        0, 
        "BiomesWebViewWindowClass",
        "Biomes Workspace Engine",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1280, 800,
        NULL, NULL, hInstance, NULL
    );

    if (!s_hwnd) return false;

    // Apply the custom frame before the first visible paint, not after resizing.
    SetWindowPos(s_hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    const DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUND;
    DwmSetWindowAttribute(s_hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
    ShowWindow(s_hwnd, nCmdShow);
    UpdateWindow(s_hwnd);

    InitWebView(startUrl);

    return true;
}

void WebViewWindow::InitWebView(const std::string&) {
    if (!loaderModule) loaderModule = LoadLibraryW((biomes::AppPaths::ExecutableDirectory() / L"WebView2Loader.dll").c_str());
    if (!loaderModule) { std::cerr << "[WEBVIEW] Loader unavailable" << std::endl; return; }
    const auto create = reinterpret_cast<PFN_CreateCoreWebView2EnvironmentWithOptions>(
        GetProcAddress(loaderModule, "CreateCoreWebView2EnvironmentWithOptions"));
    if (!create) return;
    const auto generation = hostGeneration;
    auto environmentHandler = CreateCallbackRaw<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler,
        HRESULT, ICoreWebView2Environment*>([generation](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
        if (shuttingDown || !s_hwnd || generation != hostGeneration) return S_OK;
        if (FAILED(result) || !env) return FAILED(result) ? result : E_FAIL;
        auto controllerHandler = CreateCallbackRaw<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler,
            HRESULT, ICoreWebView2Controller*>([generation](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
            if (shuttingDown || !s_hwnd || generation != hostGeneration) {
                if (controller) controller->Close(); return S_OK;
            }
            if (FAILED(result) || !controller) return FAILED(result) ? result : E_FAIL;
            s_controller = controller; s_controller->AddRef();
            if (FAILED(controller->get_CoreWebView2(&s_webview)) || !s_webview) {
                controller->Close(); s_controller->Release(); s_controller = nullptr; return E_FAIL;
            }
            RECT bounds{}; GetClientRect(s_hwnd, &bounds);
            controller->put_Bounds(bounds);
            controller->put_IsVisible(IsWindowVisible(s_hwnd) && !IsIconic(s_hwnd));
            auto received = CreateCallbackRaw<ICoreWebView2WebMessageReceivedEventHandler,
                ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs*>(
                [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                    if (shuttingDown) return S_OK;
                    LPWSTR source = nullptr;
                    if (FAILED(args->get_Source(&source)) || !source) return S_OK;
                    std::wstring origin(source); CoTaskMemFree(source);
                    origin.resize(origin.find_first_of(L"?#") == std::wstring::npos ? origin.size() : origin.find_first_of(L"?#"));
                    if (origin != trustedPage) return S_OK;
                    LPWSTR text = nullptr;
                    if (FAILED(args->get_WebMessageAsJson(&text)) || !text) return S_OK;
                    std::wstring wide(text); CoTaskMemFree(text);
                    if (wide.size() > 16 * 1024 * 1024 || pendingWebMessages.size() >= 128) return E_INVALIDARG;
                    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
                        static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
                    if (count <= 0) return E_INVALIDARG;
                    std::string message(count, '\0');
                    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()),
                        message.data(), count, nullptr, nullptr);
                    pendingWebMessages.push_back(std::move(message));
                    if (!PostMessage(s_hwnd, kDispatchWebMessage, 0, 0)) pendingWebMessages.pop_back();
                    return S_OK;
                });
            const HRESULT messageResult = s_webview->add_WebMessageReceived(received, &messageToken);
            received->Release();
            auto navigating = CreateCallbackRaw<ICoreWebView2NavigationStartingEventHandler,
                ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs*>(
                [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                    LPWSTR uri = nullptr;
                    if (FAILED(args->get_Uri(&uri)) || !uri) { args->put_Cancel(TRUE); return S_OK; }
                    std::wstring url(uri); CoTaskMemFree(uri);
                    url = url.substr(0, url.find_first_of(L"?#"));
                    if (url != trustedPage) { args->put_Cancel(TRUE); return S_OK; }
                    pageReady = false; pendingWebMessages.clear();
                    return S_OK;
                });
            const HRESULT navigationResult = s_webview->add_NavigationStarting(navigating, &navigationToken);
            navigating->Release();
            auto completed = CreateCallbackRaw<ICoreWebView2NavigationCompletedEventHandler,
                ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*>(
                [](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                    BOOL success = FALSE; args->get_IsSuccess(&success);
                    if (!shuttingDown && success) PostMessage(s_hwnd, kPageReady, 0, 0);
                    return S_OK;
                });
            const HRESULT completedResult = s_webview->add_NavigationCompleted(completed, &completedToken);
            completed->Release();
            if (FAILED(messageResult) || FAILED(navigationResult) || FAILED(completedResult)) return E_FAIL;
            return s_webview->Navigate(trustedPage.c_str());
        });
        const HRESULT resultController = env->CreateCoreWebView2Controller(s_hwnd, controllerHandler);
        controllerHandler->Release();
        return resultController;
    });
    const HRESULT result = create(nullptr, biomes::AppPaths::WebViewData().c_str(), nullptr, environmentHandler);
    environmentHandler->Release();
    if (FAILED(result)) std::cerr << "[WEBVIEW] Environment creation failed: " << result << std::endl;
}

void WebViewWindow::SendMessageToUI(const std::string& jsonPayload) {
    if (shuttingDown || !s_webview || !pageReady || jsonPayload.empty()) return;
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, jsonPayload.data(),
        static_cast<int>(jsonPayload.size()), nullptr, 0);
    if (count <= 0) return;
    std::wstring payload(count, '\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, jsonPayload.data(), static_cast<int>(jsonPayload.size()), payload.data(), count);
    const HRESULT result = s_webview->PostWebMessageAsJson(payload.c_str());
    if (FAILED(result)) std::cerr << "[IPC] PostWebMessageAsJson failed: " << result << std::endl;
}

HWND WebViewWindow::GetSnapTarget() {
    RememberAppWindow(GetForegroundWindow());
    DWORD pid = 0;
    if (IsWindow(lastAppWindow)) GetWindowThreadProcessId(lastAppWindow, &pid);
    return pid && pid == lastAppPid ? lastAppWindow : nullptr;
}
void WebViewWindow::SetPageReadyCallback(std::function<void()> callback) { pageReadyCallback = std::move(callback); }

void WebViewWindow::SetMessageReceivedCallback(std::function<void(const std::string&)> callback) {
    s_onMessageReceived = callback;
}

void WebViewWindow::SetHotkeyPressedCallback(std::function<void(int)> callback) {
    s_onHotkeyPressed = callback;
}

void WebViewWindow::SetDisplayChangedCallback(std::function<void()> callback) {
    s_onDisplayChanged = callback;
}

HWND WebViewWindow::GetHwnd() {
    return s_hwnd;
}

void WebViewWindow::RunMessageLoop() {
    MSG msg = {};
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
}

LRESULT CALLBACK WebViewWindow::WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case kPageReady:
            if (!shuttingDown && s_webview) {
                pageReady = true;
                try { if (pageReadyCallback) pageReadyCallback(); } catch (...) {}
                if (!pendingWebMessages.empty()) PostMessage(hwnd, kDispatchWebMessage, 0, 0);
            }
            return 0;
        case WM_CLOSE:
            if (!shuttingDown && closeRequested) closeRequested();
            return 0;
        case kDispatchWebMessage: {
            if (!pageReady || shuttingDown || dispatchingWebMessage || pendingWebMessages.empty()) return 0;
            std::string message = std::move(pendingWebMessages.front());
            pendingWebMessages.pop_front();
            dispatchingWebMessage = true;
            try { if (s_onMessageReceived) s_onMessageReceived(message); }
            catch (const std::exception& error) { std::cerr << "[IPC] " << error.what() << std::endl; }
            catch (...) { std::cerr << "[IPC] Unexpected native handler exception" << std::endl; }
            dispatchingWebMessage = false;
            if (!pendingWebMessages.empty()) PostMessage(hwnd, kDispatchWebMessage, 0, 0);
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto* limits = reinterpret_cast<MINMAXINFO*>(lParam);
            const UINT dpi = GetDpiForWindow(hwnd);
            MONITORINFO monitor{sizeof(MONITORINFO)};
            const bool known = GetMonitorInfo(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor) != FALSE;
            limits->ptMinTrackSize.x = (std::min<LONG>)(MulDiv(760, dpi, 96), known ? monitor.rcWork.right - monitor.rcWork.left : MAXLONG);
            limits->ptMinTrackSize.y = (std::min<LONG>)(MulDiv(560, dpi, 96), known ? monitor.rcWork.bottom - monitor.rcWork.top : MAXLONG);
            return 0;
        }
        case WM_NCCALCSIZE:
            // Both creation (RECT) and subsequent sizing (NCCALCSIZE_PARAMS)
            // use an edge-to-edge client area. Never reserve a caption strip.
            if (IsZoomed(hwnd)) {
                auto* rect = wParam
                    ? &reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam)->rgrc[0]
                    : reinterpret_cast<RECT*>(lParam);
                MONITORINFO monitor{sizeof(MONITORINFO)};
                if (GetMonitorInfo(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor))
                    *rect = monitor.rcWork;
            }
            return 0;
        case WM_NCHITTEST: {
            if (IsZoomed(hwnd)) return HTCLIENT;
            RECT rect{};
            GetWindowRect(hwnd, &rect);
            const int edge = MulDiv(6, GetDpiForWindow(hwnd), 96);
            const int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
            const bool left = x < rect.left + edge, right = x >= rect.right - edge;
            const bool top = y < rect.top + edge, bottom = y >= rect.bottom - edge;
            if (top) return left ? HTTOPLEFT : right ? HTTOPRIGHT : HTTOP;
            if (bottom) return left ? HTBOTTOMLEFT : right ? HTBOTTOMRIGHT : HTBOTTOM;
            return left ? HTLEFT : right ? HTRIGHT : HTCLIENT;
        }
        case WM_DPICHANGED: {
            const auto* rect = reinterpret_cast<RECT*>(lParam);
            SetWindowPos(hwnd, nullptr, rect->left, rect->top,
                         rect->right - rect->left, rect->bottom - rect->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_HOTKEY:
            if (s_onHotkeyPressed) s_onHotkeyPressed(static_cast<int>(wParam));
            break;
        case WM_DISPLAYCHANGE:
            if (s_onDisplayChanged) s_onDisplayChanged();
            break;
        case WM_SETTINGCHANGE:
            if (wParam == SPI_SETWORKAREA && s_onDisplayChanged) {
                s_onDisplayChanged();
            }
            break;
        case WM_SIZE:
            if (s_controller != nullptr) {
                if (wParam == SIZE_MINIMIZED) {
                    // WebView2 can wedge if left "visible" while the host is minimized.
                    s_controller->put_IsVisible(FALSE);
                } else {
                    s_controller->put_IsVisible(IsWindowVisible(hwnd));
                    RECT bounds;
                    GetClientRect(hwnd, &bounds);
                    s_controller->put_Bounds(bounds);
                }
            }
            break;
        case WM_SHOWWINDOW:
            if (s_controller) s_controller->put_IsVisible(wParam && !IsIconic(hwnd));
            break;
        case WM_MOVE:
            if (s_controller) s_controller->NotifyParentWindowPositionChanged();
            break;
        case WM_ACTIVATE:
            if (LOWORD(wParam) != WA_INACTIVE) RememberAppWindow(reinterpret_cast<HWND>(lParam));
            if (LOWORD(wParam) != WA_INACTIVE && s_controller) {
                s_controller->put_IsVisible(TRUE);
                RECT bounds;
                GetClientRect(hwnd, &bounds);
                s_controller->put_Bounds(bounds);
            }
            break;
        case WM_DESTROY:
            ++hostGeneration; pageReady = false;
            pendingWebMessages.clear();
            if (s_webview) {
                s_webview->remove_WebMessageReceived(messageToken);
                s_webview->remove_NavigationStarting(navigationToken);
                s_webview->remove_NavigationCompleted(completedToken);
            }
            if (s_controller) s_controller->Close();
            if (s_webview) { s_webview->Release(); s_webview = nullptr; }
            if (s_controller) { s_controller->Release(); s_controller = nullptr; }
            s_hwnd = nullptr;
            break;
        default:
            return DefWindowProc(hwnd, uMsg, wParam, lParam);
    }
    return 0;
}

void WebViewWindow::RestoreDashboard() {
    if (shuttingDown) return;
    if (!s_hwnd && showRequested) { showRequested(); return; }
    if (!s_hwnd || !IsWindow(s_hwnd)) return;

    RememberAppWindow(GetForegroundWindow());
    EnableWindow(s_hwnd, TRUE);

    if (IsIconic(s_hwnd)) ShowWindow(s_hwnd, SW_RESTORE);
    else ShowWindow(s_hwnd, SW_SHOW);
    SetWindowPos(s_hwnd, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);

    SetForegroundWindow(s_hwnd);
    BringWindowToTop(s_hwnd);
    UpdateWindow(s_hwnd);

    if (s_controller) {
        s_controller->put_IsVisible(TRUE);
        RECT bounds;
        GetClientRect(s_hwnd, &bounds);
        if (bounds.right > bounds.left && bounds.bottom > bounds.top) {
            s_controller->put_Bounds(bounds);
        }
    }
}

void WebViewWindow::HideDashboard() {
    if (!s_hwnd || !IsWindow(s_hwnd)) return;
    ShowWindow(s_hwnd, SW_HIDE);
    if (s_controller) s_controller->put_IsVisible(FALSE);
}

void WebViewWindow::SetShowRequestedCallback(std::function<void()> callback) { showRequested = std::move(callback); }
void WebViewWindow::SetCloseRequestedCallback(std::function<void()> callback) { closeRequested = std::move(callback); }
void WebViewWindow::Shutdown() {
    shuttingDown = true;
    showRequested = {}; closeRequested = {};
    s_onMessageReceived = {};
    pageReadyCallback = {}; pageReady = false;
    pendingWebMessages.clear();
    if (s_controller) s_controller->Close();
    if (s_hwnd) DestroyWindow(s_hwnd);
}

void WebViewWindow::MinimizeDashboard() {
    if (!s_hwnd || !IsWindow(s_hwnd)) return;
    // Prefer minimize over hide so Biomes stays on the taskbar / Alt+Tab
    // while a biome session is active (hide feels like the app closed).
    ShowWindow(s_hwnd, SW_MINIMIZE);
}
