#include "../../include/ui/webview_window.hpp"
#include <iostream>
#include <string>

#include <windows.h>
#include <initguid.h> // Resolves undefined reference to IID_IUnknown and __uuidof
#include <objbase.h>

#include "../../include/external/webview2/WebView2.h"

using namespace std;

static ICoreWebView2Controller* webviewController = nullptr;
static ICoreWebView2* webviewWindow = nullptr;

// Custom COM Handler for Controller Creation
class EnvironmentCompletedHandler : public ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler {
private:
    HWND m_hwnd;
    string m_startUrl;
    ULONG m_refCount = 1;

public:
    EnvironmentCompletedHandler(HWND hwnd, const string& startUrl) : m_hwnd(hwnd), m_startUrl(startUrl) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (riid == IID_IUnknown || riid == IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler) {
            *ppvObject = static_cast<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*>(this);
            AddRef();
            return S_OK;
        }
        *ppvObject = NULL;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_refCount); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG count = InterlockedDecrement(&m_refCount);
        if (count == 0) delete this;
        return count;
    }

    HRESULT STDMETHODCALLTYPE Invoke(HRESULT result, ICoreWebView2Environment* env) override {
        if (FAILED(result) || !env) {
            cout << "[ERROR] Environment creation failed. HRESULT: " << hex << result << endl;
            return result;
        }

        class ControllerCompletedHandler : public ICoreWebView2CreateCoreWebView2ControllerCompletedHandler {
        private:
            HWND m_hwnd;
            string m_startUrl;
            ULONG m_refCount = 1;

        public:
            ControllerCompletedHandler(HWND hwnd, const string& startUrl) : m_hwnd(hwnd), m_startUrl(startUrl) {}

            HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
                if (riid == IID_IUnknown || riid == IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler) {
                    *ppvObject = static_cast<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler*>(this);
                    AddRef();
                    return S_OK;
                }
                *ppvObject = NULL;
                return E_NOINTERFACE;
            }

            ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_refCount); }
            ULONG STDMETHODCALLTYPE Release() override {
                ULONG count = InterlockedDecrement(&m_refCount);
                if (count == 0) delete this;
                return count;
            }

            HRESULT STDMETHODCALLTYPE Invoke(HRESULT result, ICoreWebView2Controller* controller) override {
                if (controller != nullptr) {
                    webviewController = controller;
                    webviewController->get_CoreWebView2(&webviewWindow);

                    RECT bounds;
                    GetClientRect(m_hwnd, &bounds);
                    webviewController->put_Bounds(bounds);

                    wstring wUrl(m_startUrl.begin(), m_startUrl.end());
                    webviewWindow->Navigate(wUrl.c_str());

                    cout << "[UI] WebView2 loaded successfully at: " << m_startUrl << endl;
                }
                return S_OK;
            }
        };

        env->CreateCoreWebView2Controller(m_hwnd, new ControllerCompletedHandler(m_hwnd, m_startUrl));
        return S_OK;
    }
};

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_SIZE:
        if (webviewController != nullptr) {
            RECT bounds;
            GetClientRect(hWnd, &bounds);
            webviewController->put_Bounds(bounds);
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProcA(hWnd, message, wParam, lParam);
    }
    return 0;
}

// Function signature for WebView2Loader entry point
typedef HRESULT (STDAPICALLTYPE *CreateWebView2EnvFunc)(
    PCWSTR browserExecutableFolder,
    PCWSTR userDataFolder,
    ICoreWebView2EnvironmentOptions* environmentOptions,
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler* environmentCreatedHandler
);

bool WebViewWindow::Initialize(HINSTANCE hInstance, int nCmdShow, const string& startUrl) {
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    const char CLASS_NAME[] = "BiomesWebViewHost";

    WNDCLASSA wc = { 0 };
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hCursor       = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);

    RegisterClassA(&wc);

    HWND hwnd = CreateWindowExA(
        0,
        CLASS_NAME,
        "Biomes Workspace Engine",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1280, 720,
        NULL, NULL, hInstance, NULL
    );

    if (hwnd == NULL) return false;

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    cout << "[UI] Host window created. Loading WebView2Loader.dll..." << endl;

    // Load DLL dynamically
    HMODULE hLib = LoadLibraryA("WebView2Loader.dll");
    if (!hLib) {
        cout << "[ERROR] Could not load WebView2Loader.dll! Ensure WebView2Loader.dll is in the project root." << endl;
        return false;
    }

    CreateWebView2EnvFunc createEnv = (CreateWebView2EnvFunc)GetProcAddress(hLib, "CreateCoreWebView2EnvironmentWithOptions");
    if (!createEnv) {
        cout << "[ERROR] Could not locate CreateCoreWebView2EnvironmentWithOptions in DLL." << endl;
        return false;
    }

    createEnv(nullptr, nullptr, nullptr, new EnvironmentCompletedHandler(hwnd, startUrl));
    return true;
}

void WebViewWindow::RunMessageLoop() {
    MSG msg = { 0 };
    while (GetMessageA(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}