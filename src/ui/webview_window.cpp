#include <windows.h>
#include <unknwn.h>
#include <objbase.h>
#include <winerror.h>

#include <iostream>
#include <string>
#include <functional>

#include <WebView2.h>
#include "../../include/ui/webview_window.hpp"

// ============================================================================
// Function Pointer Definition for Dynamic Loading in MinGW
// ============================================================================

typedef HRESULT (__stdcall *PFN_CreateCoreWebView2EnvironmentWithOptions)(
    PCWSTR browserExecutableFolder,
    PCWSTR userDataFolder,
    ICoreWebView2EnvironmentOptions* environmentOptions,
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler* environmentCreatedHandler
);

// ============================================================================
// Native MinGW COM Callback Implementations (Zero WRL dependencies)
// ============================================================================

template <typename TInterface>
class ComCallbackImpl;

// 1. Environment Creation Callback
template <>
class ComCallbackImpl<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler> 
    : public ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler {
private:
    std::function<HRESULT(HRESULT, ICoreWebView2Environment*)> m_func;
    long m_refCount = 1;

public:
    ComCallbackImpl(std::function<HRESULT(HRESULT, ICoreWebView2Environment*)> func) 
        : m_func(std::move(func)) {}

    HRESULT __stdcall QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = static_cast<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*>(this);
        AddRef();
        return S_OK;
    }

    unsigned long __stdcall AddRef() override { return InterlockedIncrement(&m_refCount); }
    unsigned long __stdcall Release() override {
        unsigned long count = InterlockedDecrement(&m_refCount);
        if (count == 0) { delete this; return 0; }
        return count;
    }
    HRESULT __stdcall Invoke(HRESULT result, ICoreWebView2Environment* created_environment) override {
        if (m_func) return m_func(result, created_environment);
        return S_OK;
    }
};

// 2. Controller Creation Callback
template <>
class ComCallbackImpl<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler> 
    : public ICoreWebView2CreateCoreWebView2ControllerCompletedHandler {
private:
    std::function<HRESULT(HRESULT, ICoreWebView2Controller*)> m_func;
    long m_refCount = 1;

public:
    ComCallbackImpl(std::function<HRESULT(HRESULT, ICoreWebView2Controller*)> func) 
        : m_func(std::move(func)) {}

    HRESULT __stdcall QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = static_cast<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler*>(this);
        AddRef();
        return S_OK;
    }

    unsigned long __stdcall AddRef() override { return InterlockedIncrement(&m_refCount); }
    unsigned long __stdcall Release() override {
        unsigned long count = InterlockedDecrement(&m_refCount);
        if (count == 0) { delete this; return 0; }
        return count;
    }
    HRESULT __stdcall Invoke(HRESULT result, ICoreWebView2Controller* created_controller) override {
        if (m_func) return m_func(result, created_controller);
        return S_OK;
    }
};

// 3. Web Message Received Callback (IPC)
template <>
class ComCallbackImpl<ICoreWebView2WebMessageReceivedEventHandler> 
    : public ICoreWebView2WebMessageReceivedEventHandler {
private:
    std::function<HRESULT(ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs*)> m_func;
    long m_refCount = 1;

public:
    ComCallbackImpl(std::function<HRESULT(ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs*)> func) 
        : m_func(std::move(func)) {}

    HRESULT __stdcall QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = static_cast<ICoreWebView2WebMessageReceivedEventHandler*>(this);
        AddRef();
        return S_OK;
    }

    unsigned long __stdcall AddRef() override { return InterlockedIncrement(&m_refCount); }
    unsigned long __stdcall Release() override {
        unsigned long count = InterlockedDecrement(&m_refCount);
        if (count == 0) { delete this; return 0; }
        return count;
    }
    HRESULT __stdcall Invoke(ICoreWebView2* sender, ICoreWebView2WebMessageReceivedEventArgs* args) override {
        if (m_func) return m_func(sender, args);
        return S_OK;
    }
};

template <typename TInterface, typename F>
TInterface* CreateCallbackRaw(F&& func) {
    return new ComCallbackImpl<TInterface>(std::forward<F>(func));
}

// ============================================================================
// Static Member Definitions (Matches include/ui/webview_window.hpp)
// ============================================================================

HWND WebViewWindow::s_hwnd = nullptr;
ICoreWebView2Controller* WebViewWindow::s_controller = nullptr;
ICoreWebView2* WebViewWindow::s_webview = nullptr;
std::function<void(const std::string&)> WebViewWindow::s_onMessageReceived = nullptr;

bool WebViewWindow::Initialize(HINSTANCE hInstance, int nCmdShow, const std::string& startUrl) {
    WNDCLASSEXA wc = { sizeof(WNDCLASSEXA) };
    wc.lpfnWndProc   = WindowProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = "BiomesWebViewWindowClass";
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);

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

    ShowWindow(s_hwnd, nCmdShow);
    UpdateWindow(s_hwnd);

    InitWebView(startUrl);

    return true;
}

void WebViewWindow::InitWebView(const std::string& startUrl) {
    HMODULE hLoader = LoadLibraryW(L"WebView2Loader.dll");
    if (!hLoader) {
        std::cerr << "[WEBVIEW ERROR] Could not load WebView2Loader.dll!" << std::endl;
        return;
    }

    auto pfnCreateEnvironment = reinterpret_cast<PFN_CreateCoreWebView2EnvironmentWithOptions>(
        GetProcAddress(hLoader, "CreateCoreWebView2EnvironmentWithOptions")
    );

    if (!pfnCreateEnvironment) {
        std::cerr << "[WEBVIEW ERROR] Could not locate CreateCoreWebView2EnvironmentWithOptions inside DLL." << std::endl;
        return;
    }

    std::wstring userDataFolder = L"webview_data";

    auto envHandler = CreateCallbackRaw<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
        [startUrl](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
            if (FAILED(result) || !env) return result;

            auto controllerHandler = CreateCallbackRaw<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                [startUrl](HRESULT res, ICoreWebView2Controller* controller) -> HRESULT {
                    if (FAILED(res) || !controller) return res;

                    s_controller = controller;
                    s_controller->AddRef();
                    
                    s_controller->get_CoreWebView2(&s_webview);

                    RECT bounds;
                    GetClientRect(s_hwnd, &bounds);
                    s_controller->put_Bounds(bounds);

                    EventRegistrationToken token;
                    auto msgHandler = CreateCallbackRaw<ICoreWebView2WebMessageReceivedEventHandler>(
                        [](ICoreWebView2* sender, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                            LPWSTR jsonString = nullptr;
                            if (SUCCEEDED(args->get_WebMessageAsJson(&jsonString)) && jsonString) {
                                std::wstring wMsg(jsonString);
                                CoTaskMemFree(jsonString);

                                std::string msg(wMsg.begin(), wMsg.end());
                                if (s_onMessageReceived) s_onMessageReceived(msg);
                            }
                            return S_OK;
                        }
                    );
                    s_webview->add_WebMessageReceived(msgHandler, &token);

                    std::wstring wUrl(startUrl.begin(), startUrl.end());
                    s_webview->Navigate(wUrl.c_str());

                    std::cout << "[WEBVIEW] Successfully loaded: " << startUrl << std::endl;
                    return S_OK;
                }
            );

            env->CreateCoreWebView2Controller(s_hwnd, controllerHandler);
            return S_OK;
        }
    );

    // CALL THE DYNAMIC POINTER (Fixes linker error)
    pfnCreateEnvironment(nullptr, userDataFolder.c_str(), nullptr, envHandler);
}

void WebViewWindow::SendMessageToUI(const std::string& jsonPayload) {
    if (s_webview) {
        std::wstring wPayload(jsonPayload.begin(), jsonPayload.end());
        s_webview->PostWebMessageAsJson(wPayload.c_str());
        std::cout << "[IPC C++ -> JS] Sent payload: " << jsonPayload << std::endl;
    }
}

void WebViewWindow::SetMessageReceivedCallback(std::function<void(const std::string&)> callback) {
    s_onMessageReceived = callback;
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
        case WM_SIZE:
            if (s_controller != nullptr) {
                RECT bounds;
                GetClientRect(hwnd, &bounds);
                s_controller->put_Bounds(bounds);
            }
            break;
        case WM_DESTROY:
            if (s_webview) { s_webview->Release(); s_webview = nullptr; }
            if (s_controller) { s_controller->Release(); s_controller = nullptr; }
            PostQuitMessage(0);
            break;
        default:
            return DefWindowProc(hwnd, uMsg, wParam, lParam);
    }
    return 0;
}