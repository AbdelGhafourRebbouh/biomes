#define NOMINMAX
#include "../../include/ui/launch_panel.hpp"
#include "../../include/core/app_paths.hpp"
#include "../../include/external/webview2/WebView2.h"
#include "../../include/external/nlohmann/json.hpp"
#include <dcomp.h>
#include <windowsx.h>
#include <shellapi.h>
#include <filesystem>
#include <functional>
#include <fstream>
#include <deque>
#include <algorithm>
#ifdef BIOMES_PANEL_TESTING
#include <shlwapi.h>
#endif

namespace {
using json = nlohmann::json;
template<class I, class... A> class Handler final : public I {
    LONG refs = 1;
    std::function<HRESULT(A...)> fn;
public:
    explicit Handler(std::function<HRESULT(A...)> callback) : fn(std::move(callback)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (id != __uuidof(IUnknown) && id != __uuidof(I)) return E_NOINTERFACE;
        *out = static_cast<I*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs); }
    ULONG STDMETHODCALLTYPE Release() override { const LONG left = InterlockedDecrement(&refs); if (!left) delete this; return left; }
    HRESULT STDMETHODCALLTYPE Invoke(A... args) override { try { return fn(args...); } catch (...) { return E_FAIL; } }
};
template<class T> void Release(T*& value) { if (value) { value->Release(); value = nullptr; } }
using EnvironmentHandler = Handler<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler, HRESULT, ICoreWebView2Environment*>;
using CompositionHandler = Handler<ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler, HRESULT, ICoreWebView2CompositionController*>;
using MessageHandler = Handler<ICoreWebView2WebMessageReceivedEventHandler, ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs*>;
using NavigationHandler = Handler<ICoreWebView2NavigationStartingEventHandler, ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs*>;
using NewWindowHandler = Handler<ICoreWebView2NewWindowRequestedEventHandler, ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs*>;
HWND panelWindow = nullptr, dashboardWindow = nullptr;
ICoreWebView2CompositionController* composition = nullptr;
ICoreWebView2Controller* controller = nullptr;
ICoreWebView2* webview = nullptr;
IDCompositionDevice* device = nullptr;
IDCompositionTarget* target = nullptr;
IDCompositionVisual* visual = nullptr;
HMODULE loader = nullptr;
bool shuttingDown = false, ready = false, trackingMouse = false;
json latest;
std::string theme = "light";
unsigned long long dismissedSession = 0;
RECT hitBounds{};
std::wstring pageUrl;
std::deque<std::string> commands;
#ifdef BIOMES_PANEL_TESTING
bool testReportReceived = false;
bool testExpectReport = false;
#endif
constexpr UINT kUpdate = WM_APP + 110, kCommand = WM_APP + 111;

std::wstring Wide(const std::string& text) {
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(size, L'\0');
    if (size) MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}
std::string Utf8(const wchar_t* text) {
    const int length = static_cast<int>(wcslen(text));
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    if (size) WideCharToMultiByte(CP_UTF8, 0, text, length, result.data(), size, nullptr, nullptr);
    return result;
}
void Log(const char* stage, HRESULT hr) {
    std::ofstream file(biomes::AppPaths::RuntimeLog(), std::ios::app);
    file << "[LAUNCH PANEL] " << stage << " HRESULT=" << std::hex << static_cast<unsigned long>(hr) << '\n';
}
void Post(const json& message) {
    if (webview && ready) webview->PostWebMessageAsJson(Wide(message.dump()).c_str());
}
void Position(bool selectMonitor) {
    if (!panelWindow) return;
    static HMONITOR monitorHandle = nullptr;
    if (selectMonitor || !monitorHandle)
        monitorHandle = MonitorFromWindow(dashboardWindow, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO monitor{sizeof(monitor)};
    if (!GetMonitorInfoW(monitorHandle, &monitor)) {
        monitorHandle = MonitorFromWindow(dashboardWindow, MONITOR_DEFAULTTOPRIMARY);
        if (!GetMonitorInfoW(monitorHandle, &monitor)) return;
    }
    UINT dpi = GetDpiForWindow(panelWindow);
    if (!dpi) dpi = 96;
    const int width = std::min<LONG>(MulDiv(520, dpi, 96), monitor.rcWork.right - monitor.rcWork.left);
    const int gap = std::min<LONG>(MulDiv(24, dpi, 96), (monitor.rcWork.bottom - monitor.rcWork.top) / 10);
    const int height = std::min<LONG>(MulDiv(560, dpi, 96), monitor.rcWork.bottom - monitor.rcWork.top - gap);
    SetWindowPos(panelWindow, HWND_TOPMOST, monitor.rcWork.left + (monitor.rcWork.right-monitor.rcWork.left-width)/2,
        monitor.rcWork.bottom-gap-height, width, height, SWP_NOACTIVATE);
    if (controller) {
        RECT bounds{}; GetClientRect(panelWindow, &bounds);
        controller->put_Bounds(bounds);
        controller->NotifyParentWindowPositionChanged();
    }
}
void Flush() {
    if (!ready || latest.empty() || !panelWindow) return;
    Post({{"action", "THEME"}, {"theme", theme}});
    if (latest.value("state", "") == "cancelled") { Post(latest); return; }
    if (latest.value("session", 0ULL) == dismissedSession) return;
    Post(latest);
    Position(false);
    ShowWindow(panelWindow, SW_SHOWNOACTIVATE);
    if (controller) controller->put_IsVisible(TRUE);
}
void HandleCommand(const std::string& message) {
    const auto request = json::parse(message);
    const auto action = request.value("action", "");
    if (action == "READY") { ready = true; Flush(); return; }
    if (action == "BOUNDS") {
        const double scale = static_cast<double>(GetDpiForWindow(panelWindow)) / 96.0;
        hitBounds = {static_cast<LONG>(request.value("x", 0.0)*scale), static_cast<LONG>(request.value("y", 0.0)*scale),
            static_cast<LONG>((request.value("x", 0.0)+request.value("width", 0.0))*scale),
            static_cast<LONG>((request.value("y", 0.0)+request.value("height", 0.0))*scale)};
        return;
    }
    if (latest.empty() || request.value("session", 0ULL) != latest.value("session", 0ULL)) return;
    if (action == "DISMISS") {
        dismissedSession = request.value("session", 0ULL);
        ShowWindow(panelWindow, SW_HIDE);
        if (controller) controller->put_IsVisible(FALSE);
    } else if (action == "REPORT") {
#ifdef BIOMES_PANEL_TESTING
        testReportReceived = true;
#else
        // Open a blank issue composer only. Never upload logs, paths, or app data.
        ShellExecuteW(nullptr, L"open", L"https://github.com/AbdelGhafourRebbouh/biomes/issues/new", nullptr, nullptr, SW_SHOWNORMAL);
#endif
    }
}
LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    switch (message) {
    case kUpdate: Flush(); return 0;
    case kCommand:
        if (!commands.empty()) {
            auto command = std::move(commands.front()); commands.pop_front();
            try { HandleCommand(command); } catch (...) { Log("invalid panel command", E_INVALIDARG); }
        }
        return 0;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_NCHITTEST: {
        POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}; ScreenToClient(hwnd, &point);
        return PtInRect(&hitBounds, point) ? HTCLIENT : HTTRANSPARENT;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT paint{}; BeginPaint(hwnd, &paint); EndPaint(hwnd, &paint); return 0; }
    case WM_DISPLAYCHANGE: Position(true); return 0;
    case WM_DPICHANGED: Position(false); return 0;
    case WM_SIZE:
        if (controller) { RECT rect{}; GetClientRect(hwnd, &rect); controller->put_Bounds(rect); }
        return 0;
    case WM_CLOSE: ShowWindow(hwnd, SW_HIDE); return 0;
    case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_MOUSEWHEEL: case WM_MOUSELEAVE: {
        if (!composition) return 0;
        POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (message == WM_MOUSEWHEEL) ScreenToClient(hwnd, &point);
        if (message == WM_MOUSEMOVE && !trackingMouse) {
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0}; TrackMouseEvent(&track); trackingMouse = true;
        }
        if (message == WM_MOUSELEAVE) trackingMouse = false;
        if (message == WM_LBUTTONDOWN) SetCapture(hwnd);
        if (message == WM_LBUTTONUP && GetCapture() == hwnd) ReleaseCapture();
        composition->SendMouseInput(static_cast<COREWEBVIEW2_MOUSE_EVENT_KIND>(message),
            static_cast<COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS>(GET_KEYSTATE_WPARAM(wp)),
            message == WM_MOUSEWHEEL ? static_cast<UINT32>(GET_WHEEL_DELTA_WPARAM(wp)) : 0, point);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}
}

void LaunchPanel::Initialize(HWND dashboard) {
    if (panelWindow) return;
    shuttingDown = false; dashboardWindow = dashboard;
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW wc{}; wc.lpfnWndProc = WindowProc; wc.hInstance = instance;
    wc.lpszClassName = L"BiomesLaunchPanel"; wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    RegisterClassW(&wc);
    panelWindow = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP,
        wc.lpszClassName, L"biomes launch progress", WS_POPUP, 0, 0, 520, 440, nullptr, nullptr, instance, nullptr);
    if (!panelWindow) { Log("CreateWindow", HRESULT_FROM_WIN32(GetLastError())); return; }
    HRESULT hr = DCompositionCreateDevice(nullptr, __uuidof(IDCompositionDevice), reinterpret_cast<void**>(&device));
    if (FAILED(hr)) { Log("DirectComposition device", hr); return; }
    hr = device->CreateTargetForHwnd(panelWindow, TRUE, &target);
    if (SUCCEEDED(hr)) hr = device->CreateVisual(&visual);
    if (SUCCEEDED(hr)) hr = target->SetRoot(visual);
    if (FAILED(hr)) { Log("DirectComposition tree", hr); return; }
    device->Commit();
    wchar_t executable[32768]{}; GetModuleFileNameW(nullptr, executable, 32768);
    const auto directory = std::filesystem::path(executable).parent_path();
    pageUrl = L"file:///" + (directory / L"launch-panel.html").generic_wstring();
    loader = LoadLibraryW((directory / L"WebView2Loader.dll").c_str());
    if (!loader) { Log("WebView2 loader", HRESULT_FROM_WIN32(GetLastError())); return; }
    using CreateEnvironment = HRESULT(STDAPICALLTYPE*)(PCWSTR, PCWSTR, ICoreWebView2EnvironmentOptions*, ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*);
    auto create = reinterpret_cast<CreateEnvironment>(GetProcAddress(loader, "CreateCoreWebView2EnvironmentWithOptions"));
    if (!create) { Log("environment entry point", E_FAIL); return; }
    auto handler = new EnvironmentHandler([](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
        if (shuttingDown) return S_OK;
        if (FAILED(result) || !environment) { Log("environment", result); return result; }
        ICoreWebView2Environment3* environment3 = nullptr;
        HRESULT hr = environment->QueryInterface(__uuidof(ICoreWebView2Environment3), reinterpret_cast<void**>(&environment3));
        if (FAILED(hr)) { Log("composition capability", hr); return hr; }
        auto completed = new CompositionHandler([](HRESULT result, ICoreWebView2CompositionController* created) -> HRESULT {
            if (shuttingDown) return S_OK;
            if (FAILED(result) || !created) { Log("composition controller", result); return result; }
            composition = created; composition->AddRef();
            HRESULT hr = composition->QueryInterface(__uuidof(ICoreWebView2Controller), reinterpret_cast<void**>(&controller));
            if (FAILED(hr)) return hr;
            hr = controller->get_CoreWebView2(&webview); if (FAILED(hr)) return hr;
            ICoreWebView2Controller2* controller2 = nullptr;
            hr = controller->QueryInterface(__uuidof(ICoreWebView2Controller2), reinterpret_cast<void**>(&controller2));
            if (FAILED(hr)) { Log("transparent background capability", hr); return hr; }
            controller2->put_DefaultBackgroundColor({0, 0, 0, 0}); controller2->Release();
            composition->put_RootVisualTarget(visual); device->Commit();
            ICoreWebView2Settings* settings = nullptr;
            if (SUCCEEDED(webview->get_Settings(&settings))) {
                settings->put_AreDefaultContextMenusEnabled(FALSE); settings->put_AreDevToolsEnabled(FALSE);
                settings->put_IsStatusBarEnabled(FALSE); settings->put_IsZoomControlEnabled(FALSE);
                settings->Release();
            }
            EventRegistrationToken token{};
            auto messages = new MessageHandler([](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                LPWSTR source = nullptr; args->get_Source(&source);
                const bool trusted = source && pageUrl == source; CoTaskMemFree(source);
                if (!trusted || shuttingDown) return S_OK;
                LPWSTR raw = nullptr;
                if (SUCCEEDED(args->get_WebMessageAsJson(&raw)) && raw) {
                    const std::string command = Utf8(raw); CoTaskMemFree(raw);
                    commands.push_back(command); PostMessageW(panelWindow, kCommand, 0, 0);
                }
                return S_OK;
            });
            webview->add_WebMessageReceived(messages, &token); messages->Release();
            auto navigation = new NavigationHandler([](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                LPWSTR uri = nullptr; args->get_Uri(&uri);
                if (!uri || pageUrl != uri) args->put_Cancel(TRUE);
                CoTaskMemFree(uri); return S_OK;
            });
            webview->add_NavigationStarting(navigation, &token); navigation->Release();
            auto newWindow = new NewWindowHandler([](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                return args->put_Handled(TRUE);
            });
            webview->add_NewWindowRequested(newWindow, &token); newWindow->Release();
            Position(true);
            return webview->Navigate(pageUrl.c_str());
        });
        hr = environment3->CreateCoreWebView2CompositionController(panelWindow, completed);
        completed->Release(); environment3->Release(); return hr;
    });
    const auto profile = biomes::AppPaths::WebViewData().wstring();
    hr = create(nullptr, profile.c_str(), nullptr, handler); handler->Release();
    if (FAILED(hr)) Log("environment request", hr);
}
void LaunchPanel::Update(const std::string& payload) {
    try {
        auto update = json::parse(payload);
        if (update.value("action", "") != "LAUNCH_PROGRESS") return;
        if (latest.empty() || update.value("session", 0ULL) != latest.value("session", 0ULL)) Position(true);
        latest = std::move(update);
        if (panelWindow) PostMessageW(panelWindow, kUpdate, 0, 0);
    } catch (...) { Log("progress serialization", E_INVALIDARG); }
}
void LaunchPanel::SetTheme(const std::string& value) {
    theme = value == "dark" ? "dark" : "light";
    if (panelWindow) PostMessageW(panelWindow, kUpdate, 0, 0);
}
void LaunchPanel::Shutdown() {
    shuttingDown = true; ready = false; commands.clear();
    if (controller) controller->Close();
    if (composition) composition->put_RootVisualTarget(nullptr);
    Release(webview); Release(controller); Release(composition);
    Release(visual); Release(target); Release(device);
    if (panelWindow) DestroyWindow(panelWindow); panelWindow = nullptr;
    // Keep the loader mapped until process exit: asynchronous environment
    // callbacks can still return after shutdown and observe shuttingDown.
}
#ifdef BIOMES_PANEL_TESTING
void LaunchPanel::CaptureTestPreview(const std::wstring& path) {
    static std::wstring requestedPath;
    static int attempts = 0;
    static bool settled = false;
    requestedPath = path;
    if (!webview || !ready || !settled) {
        if (++attempts > 30) { Log("preview not ready", E_PENDING); PostQuitMessage(2); return; }
        if (webview && ready) settled = true;
        SetTimer(nullptr, 0, 600, [](HWND, UINT, UINT_PTR timer, DWORD) {
            KillTimer(nullptr, timer); LaunchPanel::CaptureTestPreview(requestedPath);
        });
        return;
    }
    const LONG_PTR style = GetWindowLongPtrW(panelWindow, GWL_EXSTYLE);
    RECT panelRect{}; GetWindowRect(panelWindow, &panelRect);
    MONITORINFO testMonitor{sizeof(testMonitor)};
    const bool aboveTaskbar = GetMonitorInfoW(MonitorFromWindow(panelWindow, MONITOR_DEFAULTTOPRIMARY), &testMonitor) &&
        panelRect.bottom < testMonitor.rcWork.bottom && panelRect.top >= testMonitor.rcWork.top;
    const bool nativeSurfaceValid = (style & WS_EX_NOACTIVATE) && (style & WS_EX_TOPMOST) &&
        (style & WS_EX_NOREDIRECTIONBITMAP) && (style & WS_EX_TOOLWINDOW) &&
        GetAncestor(GetForegroundWindow(), GA_ROOT) != panelWindow && aboveTaskbar;
    if (!nativeSurfaceValid) { Log("native surface invariants", E_FAIL); PostQuitMessage(7); return; }
    Log("native surface invariants", S_OK);
    IStream* stream = nullptr;
    HRESULT hr = SHCreateStreamOnFileEx(path.c_str(), STGM_CREATE | STGM_WRITE | STGM_SHARE_EXCLUSIVE,
        FILE_ATTRIBUTE_NORMAL, TRUE, nullptr, &stream);
    if (FAILED(hr)) { Log("preview stream", hr); PostQuitMessage(3); return; }
    auto completed = new Handler<ICoreWebView2CapturePreviewCompletedHandler, HRESULT>([stream](HRESULT result) -> HRESULT {
        stream->Commit(STGC_DEFAULT); stream->Release();
        Log("preview capture", result);
        if (FAILED(result)) { PostQuitMessage(4); return S_OK; }
        // Exercise actual page handlers and IPC without opening an external
        // browser. Production REPORT still opens the fixed GitHub issue URL.
        testExpectReport = latest.value("state", "") == "partial";
        if (testExpectReport)
            webview->ExecuteScript(L"document.querySelector('.report').click(); document.querySelector('.continue').click();", nullptr);
        else if (latest.value("state", "") == "opening") {
            auto cancelled = latest; cancelled["state"] = "cancelled";
            LaunchPanel::Update(cancelled.dump());
        }
        bool sizeNote = false;
        for (const auto& item : latest.value("items", json::array()))
            sizeNote = sizeNote || item.value("state", "") == "constrained";
        // Allow the full readable success dwell plus the garage exit, even
        // when WebView2 becomes ready immediately before the capture.
        const UINT dismissWait = sizeNote ? 5500 : latest.value("state", "") == "success" ? 3500 : 1100;
        SetTimer(nullptr, 0, dismissWait, [](HWND, UINT, UINT_PTR timer, DWORD) {
            KillTimer(nullptr, timer);
            const bool passed = (!testExpectReport || testReportReceived) && !IsWindowVisible(panelWindow);
            Log("button IPC smoke test", passed ? S_OK : E_FAIL);
            PostQuitMessage(passed ? 0 : 6);
        });
        return S_OK;
    });
    hr = webview->CapturePreview(COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG, stream, completed);
    completed->Release();
    if (FAILED(hr)) { stream->Release(); Log("preview request", hr); PostQuitMessage(5); }
}
#endif
