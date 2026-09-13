// Callback ABI checks and WebView2 integration with an isolated temporary profile.
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
        struct ViewGuard { ICoreWebView2* value; ~ViewGuard() { if (value) value->Release(); } } viewGuard{dashboard};
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
        // Real animation sampling needs a visible controller (hidden pages throttle timers).
        ShowWindow(WebViewWindow::GetHwnd(), SW_SHOWNOACTIVATE);
        retained->put_IsVisible(TRUE);
        // Exercise actual CSS transitions while native state arrives in the collapsed drawer.
        const auto checkScript = [&](const wchar_t* script) {
            bool passed = false;
            const auto deadline = GetTickCount64() + 5000;
            while (!passed && GetTickCount64() < deadline) {
                bool evaluated = false;
                auto handler = CreateCallbackRaw<ICoreWebView2ExecuteScriptCompletedHandler, HRESULT, LPCWSTR>(
                    [&](HRESULT result, LPCWSTR value) -> HRESULT {
                        passed = SUCCEEDED(result) && value && std::wstring(value) == L"true";
                        evaluated = true; return S_OK;
                    });
                const auto result = dashboard->ExecuteScript(script, handler);
                handler->Release();
                require(SUCCEEDED(result), "execute sidebar regression script");
                while (!evaluated && GetTickCount64() < deadline) {
                    MSG message{};
                    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
                    MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
                }
                require(evaluated, "sidebar regression script callback");
            }
            require(passed, "sidebar animation, native events, and beta badges");
        };
        require(SUCCEEDED(dashboard->ExecuteScript(LR"(
            document.querySelectorAll('dialog[open]').forEach(dialog => dialog.close());
            document.documentElement.dataset.theme = 'dark';
            window.sidebarRegression = false;
            (async () => {
                const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
                const toggle = document.querySelector('.sidebar-toggle');
                const shell = document.querySelector('.app-shell');
                const drawer = document.querySelector('#native-preferences-drawer');
                const nav = document.querySelector('.navigation-secondary');
                if (shell.classList.contains('is-sidebar-collapsed')) toggle.click();
                await pause(600);
                const top = nav.getBoundingClientRect().top;
                const expanded = drawer.getBoundingClientRect().height;
                toggle.click();
                let stable = true;
                for (let i = 0; i < 15; i++) {
                    await pause(20);
                    stable = stable && Math.abs(nav.getBoundingClientRect().top - top) < 1;
                }
                window.sidebarCollapsed = drawer.inert && drawer.getBoundingClientRect().height < 1 && expanded > 0;
                window.sidebarNavStable = stable;
            })();
        )", nullptr)), "start sidebar collapse");
        checkScript(L"Boolean(window.sidebarCollapsed && window.sidebarNavStable)");
        WebViewWindow::SendMessageToUI(R"({"action":"MONITOR_CHANGED","topology":{"monitors":[{}]}})");
        WebViewWindow::SendMessageToUI(R"({"action":"SETTINGS_RESULT","success":true,"settings":{"launchAtStartup":false}})");
        WebViewWindow::SendMessageToUI(R"({"action":"LOADED_BIOMES","biomes":[{"id":"beta-test","name":"Beta card","apps":[]}],"activeId":""})");
        checkScript(LR"(Boolean(document.querySelector('#native-monitor-count').textContent === '1 display connected' &&
            !document.querySelector('#native-autostart').checked && document.querySelector('#native-preferences-drawer').inert &&
            document.querySelector('.brand-beta').textContent === 'BETA' &&
            document.querySelector('#home-page .card-beta').textContent === 'BETA'))");
        require(SUCCEEDED(dashboard->ExecuteScript(LR"(
            document.querySelector('.sidebar-toggle').click();
            setTimeout(() => {
                const drawer = document.querySelector('#native-preferences-drawer');
                const css = getComputedStyle(document.querySelector('.native-preferences'));
                window.sidebarRegression = !drawer.inert && drawer.getBoundingClientRect().height > 0 && css.opacity === '1';
            }, 600);
        )", nullptr)), "expand updated sidebar");
        checkScript(L"window.sidebarRegression === true");
        wchar_t screenshotPath[32768]{};
        if (GetEnvironmentVariableW(L"BIOMES_TEST_SCREENSHOT", screenshotPath, 32768)) {
            IStream* stream = nullptr;
            require(SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)), "screenshot stream");
            bool captured = false, captureOk = false;
            auto captureHandler = CreateCallbackRaw<ICoreWebView2CapturePreviewCompletedHandler, HRESULT>(
                [&](HRESULT result) -> HRESULT { captureOk = SUCCEEDED(result); captured = true; return S_OK; });
            const auto result = dashboard->CapturePreview(COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG, stream, captureHandler);
            captureHandler->Release();
            require(SUCCEEDED(result), "capture dashboard preview");
            const auto captureDeadline = GetTickCount64() + 5000;
            while (!captured && GetTickCount64() < captureDeadline) {
                MSG message{};
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
                MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
            }
            require(captured && captureOk, "dashboard screenshot completed");
            HGLOBAL memory = nullptr; GetHGlobalFromStream(stream, &memory);
            STATSTG stat{}; stream->Stat(&stat, STATFLAG_NONAME);
            const char* bytes = static_cast<const char*>(GlobalLock(memory));
            require(bytes != nullptr, "screenshot memory");
            { std::ofstream output(std::filesystem::path(screenshotPath), std::ios::binary);
              output.write(bytes, static_cast<std::streamsize>(stat.cbSize.QuadPart));
              require(output.good(), "write dashboard screenshot"); }
            GlobalUnlock(memory); stream->Release();
        }
        WebViewWindow::HideDashboard();
        // Terminate only this test's browser process, which uses its unique temporary profile.
        const auto sentinel = biomes::AppPaths::WebViewData() / L"recovery-preservation-fixture.txt";
        { std::ofstream output(sentinel); output << "preserve"; }
        UINT32 browserPid = 0;
        require(SUCCEEDED(dashboard->get_BrowserProcessId(&browserPid)) && browserPid, "isolated browser PID");
        const HWND originalHost = WebViewWindow::GetHwnd();
        const auto oldGeneration = hostGeneration;
        loadedDashboard = false;
        dashboard->Release(); viewGuard.value = nullptr; dashboard = nullptr;
        HANDLE browser = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, browserPid);
        require(browser != nullptr, "open isolated test browser");
        const BOOL terminated = TerminateProcess(browser, 17);
        CloseHandle(browser);
        require(terminated, "simulate isolated browser crash");
        const auto recoveryDeadline = GetTickCount64() + 20000;
        while ((!loadedDashboard || hostGeneration == oldGeneration) && GetTickCount64() < recoveryDeadline) {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
        }
        require(loadedDashboard && hostGeneration > oldGeneration, "ProcessFailed recreates WebView and republishes state");
        require(WebViewWindow::GetHwnd() == originalHost && !IsWindowVisible(originalHost), "recovery preserves hidden native host");
        require(std::filesystem::exists(biomes::AppPaths::RuntimeLog()), "crash logged to isolated log");
        { std::ifstream input(sentinel); std::string value; input >> value; require(value == "preserve", "recovery retains profile files"); }
        recoveryAttempts = 3; recoveryWindow = GetTickCount64();
        // Exhaustion is bounded, and shutdown must cancel any future recreation.
        // Exercise the same path used by ProcessFailed via the native timeout.
        SendMessageW(originalHost, WM_TIMER, kCreationTimer, 0);
        require(!recoveryQueued, "recovery retry budget enforced");
        WebViewWindow::Shutdown();
        WebViewWindow::SendMessageToUI("{}");
        require(!recoveryQueued && !pageReady && !pageReadyCallback && pendingWebMessages.empty(), "shutdown clears readiness and work");
        CoUninitialize();
        std::cout << "WebView callback and integration regressions passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
