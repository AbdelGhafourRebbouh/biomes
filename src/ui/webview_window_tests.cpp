// Callback ABI checks plus a hidden WebView2 with an isolated temporary profile.
#include "webview_window.cpp"
#include <stdexcept>
#include <fstream>
#include "external/nlohmann/json.hpp"
int main() {
    try {
        auto require = [](bool value, const char* text) { if (!value) throw std::runtime_error(text); };
        int calls = 0;
        auto callback = CreateCallbackRaw<ICoreWebView2NavigationCompletedEventHandler,
            ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*>(
            [&](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT { ++calls; return S_OK; });
        void* result = reinterpret_cast<void*>(1);
        require(callback->QueryInterface(__uuidof(ICoreWebView2), &result) == E_NOINTERFACE && result == nullptr,
                "unsupported COM interface rejected");
        require(callback->QueryInterface(__uuidof(IUnknown), &result) == S_OK && result, "IUnknown supported");
        static_cast<IUnknown*>(result)->Release();
        require(callback->Invoke(nullptr, nullptr) == S_OK && calls == 1, "callback dispatch");
        require(callback->Release() == 0, "callback reference ownership");
        auto throwing = CreateCallbackRaw<ICoreWebView2NavigationCompletedEventHandler,
            ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*>(
            [](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT { throw std::runtime_error("fixture"); });
        require(throwing->Invoke(nullptr, nullptr) == E_FAIL, "exception contained at COM boundary");
        throwing->Release();
        require(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)), "COM initialization");
        const auto root = std::filesystem::temp_directory_path() /
            (L"biomes-webview-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        biomes::AppPaths::InitializeForTests(root);
        const auto html = root / L"fixture.html";
        {
            std::ofstream file(html);
            file << R"(<html><body><script>
                chrome.webview.addEventListener('message', e => {
                    if (e.data.action === 'NATIVE_STATE' && e.data.state.fixture === 42)
                        chrome.webview.postMessage({action:'STATE_ACK'});
                });
                window.addEventListener('DOMContentLoaded', () => chrome.webview.postMessage({action:'DOM_READY'}));
                </script></body></html>)";
        }
        bool domReady = false, acknowledged = false;
        WebViewWindow::SetMessageReceivedCallback([&](const std::string& message) {
            const auto parsed = nlohmann::json::parse(message);
            if (parsed.value("action", "") == "DOM_READY") domReady = true;
            if (parsed.value("action", "") == "STATE_ACK") acknowledged = true;
        });
        WebViewWindow::SetPageReadyCallback([] {
            WebViewWindow::SendMessageToUI(R"({"action":"NATIVE_STATE","state":{"fixture":42}})");
        });
        const auto fileUrl = [](const std::filesystem::path& path) {
        std::string raw = path.u8string();
        std::string url = "file:///";
        const char* hex = "0123456789ABCDEF";
        for (unsigned char c : raw) {
            if (c == '\\') url += '/';
            else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                     (c >= '0' && c <= '9') || c == ':' || c == '/' || c == '-' || c == '_' || c == '.') url += c;
            else { url += '%'; url += hex[c >> 4]; url += hex[c & 15]; }
        }
        return url;
        };
        const auto url = fileUrl(html);
        require(WebViewWindow::Initialize(GetModuleHandleW(nullptr), SW_HIDE, url), "hidden host initialization");
        const auto deadline = GetTickCount64() + 25000;
        while ((!domReady || !acknowledged) && GetTickCount64() < deadline) {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message); DispatchMessageW(&message);
            }
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
        }
        require(domReady && acknowledged, "real WebView DOM-ready JSON round trip");
        auto* retained = WebViewWindow::ControllerForTests();
        WebViewWindow::SetCloseRequestedCallback(WebViewWindow::HideDashboard);
        SendMessageW(WebViewWindow::GetHwnd(), WM_CLOSE, 0, 0);
        require(WebViewWindow::ControllerForTests() == retained && IsWindow(WebViewWindow::GetHwnd()), "close retains controller and host");
        SetWindowPos(WebViewWindow::GetHwnd(), nullptr, 0, 0, 800, 600, SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOZORDER);
        BOOL visible = TRUE; retained->get_IsVisible(&visible);
        require(!visible, "resizing hidden host does not reveal controller");
        // Load the actual shipped dashboard and verify the native-state consumer.
        ICoreWebView2* dashboard = nullptr;
        require(SUCCEEDED(retained->get_CoreWebView2(&dashboard)) && dashboard, "dashboard interface");
        struct ViewGuard { ICoreWebView2* value; ~ViewGuard() { value->Release(); } } viewGuard{dashboard};
        const auto dashboardUrl = fileUrl(biomes::AppPaths::ExecutableDirectory() / L"index.html");
        const int wideCount = MultiByteToWideChar(CP_UTF8, 0, dashboardUrl.data(), static_cast<int>(dashboardUrl.size()), nullptr, 0);
        trustedPage.resize(wideCount);
        MultiByteToWideChar(CP_UTF8, 0, dashboardUrl.data(), static_cast<int>(dashboardUrl.size()), trustedPage.data(), wideCount);
        const auto publish = [] {
            WebViewWindow::SendMessageToUI(R"({"action":"NATIVE_STATE","success":true,"state":{"settings":{"launchAtStartup":true},"topology":{"monitors":[{},{}]},"biomes":[],"activeId":""}})");
        };
        bool loadedDashboard = false;
        WebViewWindow::SetPageReadyCallback([&] { publish(); loadedDashboard = true; });
        WebViewWindow::SetMessageReceivedCallback([&](const std::string& message) {
            const auto request = nlohmann::json::parse(message);
            if (request.value("action", "") == "GET_NATIVE_STATE") publish();
        });
        require(SUCCEEDED(dashboard->Navigate(trustedPage.c_str())), "navigate to shipped dashboard");
        const auto dashboardDeadline = GetTickCount64() + 10000;
        while (!loadedDashboard && GetTickCount64() < dashboardDeadline) {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
        }
        require(loadedDashboard, "dashboard readiness");
        bool domVerified = false;
        // Retry asynchronously until the posted WebMessage has reached JS.
        while (!domVerified && GetTickCount64() < dashboardDeadline) {
            bool evaluated = false;
            auto resultHandler = CreateCallbackRaw<ICoreWebView2ExecuteScriptCompletedHandler, HRESULT, LPCWSTR>(
                [&](HRESULT result, LPCWSTR value) -> HRESULT {
                    domVerified = SUCCEEDED(result) && value && std::wstring(value) == L"true";
                    evaluated = true; return S_OK;
                });
            const HRESULT result = dashboard->ExecuteScript(
                LR"(Boolean(document.querySelector('#native-autostart')?.checked &&
                    !document.querySelector('#native-autostart')?.disabled &&
                    document.querySelector('#native-monitor-count')?.textContent === '2 displays connected'))", resultHandler);
            resultHandler->Release();
            require(SUCCEEDED(result), "query dashboard state");
            while (!evaluated && GetTickCount64() < dashboardDeadline) {
                MSG message{};
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
                MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
            }
            require(evaluated, "dashboard script callback");
        }
        require(domVerified, "native startup setting and monitor count reflected in dashboard DOM");
        WebViewWindow::Shutdown();
        WebViewWindow::SendMessageToUI("{}");
        require(!pageReady && !pageReadyCallback && pendingWebMessages.empty(), "shutdown clears readiness and work");
        CoUninitialize();
        std::cout << "WebView callback and integration regressions passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
