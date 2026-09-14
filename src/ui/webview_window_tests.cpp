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
            WebViewWindow::SendMessageToUI(R"({"action":"NATIVE_STATE","success":true,"state":{"settings":{"launchAtStartup":true,"backgroundHotkeysEnabled":true},"topology":{"monitors":[{},{}]},"biomes":[],"activeId":""}})");
        };
        bool loadedDashboard = false;
        WebViewWindow::SetPageReadyCallback([&] { publish(); loadedDashboard = true; });
        WebViewWindow::SetMessageReceivedCallback([&](const std::string& message) {
            const auto request = nlohmann::json::parse(message);
            if (request.value("action", "") == "GET_NATIVE_STATE") publish();
            if (request.value("action", "") == "ONBOARDING_WINDOW")
                WebViewWindow::SetOnboardingMode(request.at("firstRun").get<bool>());
            if (request.value("action", "") == "UPDATE_SETTINGS")
                WebViewWindow::SendMessageToUI(nlohmann::json({{"action","SETTINGS_RESULT"},{"success",true},
                    {"requestId",request.at("requestId")},{"settings",request.at("settings")}}).dump());
        });
        ShowWindow(WebViewWindow::GetHwnd(), SW_SHOWNOACTIVATE);
        retained->put_IsVisible(TRUE);
        RECT beforeOnboarding{}; GetWindowRect(WebViewWindow::GetHwnd(), &beforeOnboarding);
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
        // Verify first-run/replay modes and native settings in the actual dashboard.
        const auto checkScript = [&](const wchar_t* script) {
            bool passed = false;
            std::wstring lastResult;
            const auto deadline = GetTickCount64() + 5000;
            while (!passed && GetTickCount64() < deadline) {
                bool evaluated = false;
                auto handler = CreateCallbackRaw<ICoreWebView2ExecuteScriptCompletedHandler, HRESULT, LPCWSTR>(
                    [&](HRESULT result, LPCWSTR value) -> HRESULT {
                        passed = SUCCEEDED(result) && value && std::wstring(value) == L"true";
                        lastResult = value ? value : L"no script result";
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
            if (!passed) std::wcerr << lastResult << L'\n';
            require(passed, "settings relocation, native events, and onboarding");
        };
        checkScript(LR"(Boolean(document.body.classList.contains('is-first-run') &&
            getComputedStyle(document.querySelector('.app-shell')).visibility === 'hidden' &&
            document.querySelector('#onboarding').open &&
            document.querySelector('.onboarding-next').getBoundingClientRect().bottom <= innerHeight))");
        RECT firstRunBounds{}; GetWindowRect(WebViewWindow::GetHwnd(), &firstRunBounds);
        const auto testDpi = GetDpiForWindow(WebViewWindow::GetHwnd());
        require(firstRunBounds.right - firstRunBounds.left == MulDiv(800, testDpi, 96) &&
            firstRunBounds.bottom - firstRunBounds.top == MulDiv(520, testDpi, 96), "first-run native frame is 800x520 DIP");
        for (const auto& size : {SIZE{800,520}, SIZE{760,400}, SIZE{640,400}, SIZE{533,347}}) {
        SetWindowPos(WebViewWindow::GetHwnd(), nullptr, 0, 0,
            MulDiv(size.cx, testDpi, 96), MulDiv(size.cy, testDpi, 96), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        // Set the viewport explicitly even if native minimum tracking dimensions
        // clamp the HWND on this test machine's monitor.
        RECT viewport{0, 0, MulDiv(size.cx, testDpi, 96), MulDiv(size.cy, testDpi, 96)};
        require(SUCCEEDED(retained->put_Bounds(viewport)), "resize onboarding test viewport");
        checkScript((L"innerWidth === " + std::to_wstring(size.cx) + L" && innerHeight === " + std::to_wstring(size.cy)).c_str());
        require(SUCCEEDED(dashboard->ExecuteScript(LR"(
            window.onboardingFits = false;
            window.onboardingDiagnostics = [];
            (async () => {
                await document.fonts.ready;
                await Promise.all(document.querySelector('#onboarding').getAnimations({subtree:true}).map(a=>a.finished.catch(()=>{})));
                let fits = true;
                for (const dot of document.querySelectorAll('.onboarding-dots button')) {
                    dot.click(); await new Promise(resolve => setTimeout(resolve, 350));
                    await Promise.all(document.querySelector('#onboarding').getAnimations({subtree:true}).map(a=>a.finished.catch(()=>{})));
                    const slide = document.querySelector('.onboarding-slide').getBoundingClientRect();
                    const footer = document.querySelector('.onboarding-footer').getBoundingClientRect();
                    const top = document.querySelector('.onboarding-top').getBoundingClientRect();
                    fits = fits && footer.bottom <= innerHeight && slide.top >= top.bottom && slide.bottom <= footer.top + 1;
                    for (const selector of ['#onboarding', '.onboarding-layout', '.onboarding-copy', '.onboarding-slide']) {
                        const element = document.querySelector(selector);
                        fits = fits && element.scrollWidth <= element.clientWidth + 1 && element.scrollHeight <= element.clientHeight + 1;
                    }
                    for (const selector of ['#onboarding-title', '#onboarding-subtitle', '#onboarding-description']) {
                        const rect = document.querySelector(selector).getBoundingClientRect();
                        fits = fits && rect.top >= slide.top - 1 && rect.bottom <= slide.bottom + 1 && rect.left >= slide.left - 1 && rect.right <= slide.right + 1;
                    }
                    if (!fits) window.onboardingDiagnostics.push({card:dot.getAttribute('aria-label'), width:innerWidth, height:innerHeight,
                        boxes:[...document.querySelectorAll('#onboarding,.onboarding-layout,.onboarding-copy,.onboarding-slide,#onboarding-title,#onboarding-subtitle,#onboarding-description,.onboarding-footer')].map(e=>({name:e.id||e.className, rect:e.getBoundingClientRect().toJSON(),client:[e.clientWidth,e.clientHeight],scroll:[e.scrollWidth,e.scrollHeight]}))});
                }
                window.onboardingFits = fits;
            })();
        )", nullptr)), "check all onboarding slides");
        checkScript(L"window.onboardingFits === true || JSON.stringify(window.onboardingDiagnostics)");
        }
        SetWindowPos(WebViewWindow::GetHwnd(), nullptr, 0, 0,
            firstRunBounds.right - firstRunBounds.left, firstRunBounds.bottom - firstRunBounds.top,
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        require(SUCCEEDED(dashboard->ExecuteScript(LR"(
            document.querySelector('.onboarding-skip').click();
            document.querySelector('#replay-onboarding').click();
        )", nullptr)), "replay introduction");
        checkScript(LR"(Boolean(document.querySelector('#onboarding').open &&
            !document.body.classList.contains('is-first-run') &&
            getComputedStyle(document.querySelector('.app-shell')).visibility === 'visible'))");
        require(SUCCEEDED(dashboard->ExecuteScript(LR"(
            document.querySelector('.onboarding-skip').click();
            location.hash = '#customization';
            document.documentElement.dataset.theme = 'dark';
        )", nullptr)), "open settings");
        checkScript(LR"(Boolean(!document.querySelector('#settings-page').hidden &&
            !document.querySelector('.sidebar .native-preferences') &&
            document.querySelector('#settings-page #native-autostart') &&
            document.querySelector('#native-background-hotkeys').checked))");
        require(SUCCEEDED(dashboard->ExecuteScript(L"document.querySelector('#native-background-hotkeys').click()", nullptr)), "update background setting");
        checkScript(LR"(Boolean(!document.querySelector('#native-background-hotkeys').checked &&
            !document.querySelector('#native-background-hotkeys').disabled))");
        WebViewWindow::SendMessageToUI(R"({"action":"MONITOR_CHANGED","topology":{"monitors":[{}]}})");
        WebViewWindow::SendMessageToUI(R"({"action":"SETTINGS_RESULT","success":true,"settings":{"launchAtStartup":false}})");
        checkScript(LR"(Boolean(document.querySelector('#native-monitor-count').textContent === '1 display connected' &&
            !document.querySelector('#native-autostart').checked))");
        checkScript(L"!document.body.classList.contains('is-first-run')");
        RECT restoredBounds{}; GetWindowRect(WebViewWindow::GetHwnd(), &restoredBounds);
        require(IsWindowVisible(WebViewWindow::GetHwnd()) && restoredBounds.bottom - restoredBounds.top == beforeOnboarding.bottom - beforeOnboarding.top &&
            restoredBounds.right - restoredBounds.left == beforeOnboarding.right - beforeOnboarding.left,
            "finishing onboarding restores dashboard size and visibility");
        require(SUCCEEDED(dashboard->ExecuteScript(L"window.captureSettled=false;setTimeout(()=>window.captureSettled=true,600)", nullptr)), "settle theme transition");
        checkScript(L"window.captureSettled === true");
        require(SUCCEEDED(dashboard->ExecuteScript(LR"(
            const customBanner = document.querySelector('#settings-page .coming-banner');
            const bounds = customBanner.getBoundingClientRect();
            window.customBannerStyle = {width:bounds.width, height:bounds.height,
                radius:getComputedStyle(customBanner).borderRadius,
                font:getComputedStyle(customBanner.querySelector('h2')).fontSize};
            location.hash = '#privacy';
        )", nullptr)), "navigate to Privacy after Customization");
        checkScript(LR"(Boolean(!document.querySelector('#coming-page').hidden &&
            document.querySelector('#coming-title').textContent === 'Privacy First' &&
            document.querySelector('#coming-page .coming-kicker').textContent === 'Local & Open Source' &&
            document.querySelector('#coming-page .coming-kicker').classList.contains('privacy-badge') &&
            document.querySelector('#startup-settings-title').textContent === 'Startup & background'))");
        checkScript(LR"((() => {
            const banner = document.querySelector('#coming-page .coming-banner');
            const bounds = banner.getBoundingClientRect(), saved = window.customBannerStyle;
            return Math.abs(bounds.width-saved.width)<1 && Math.abs(bounds.height-saved.height)<1 &&
                getComputedStyle(banner).borderRadius === saved.radius &&
                getComputedStyle(banner.querySelector('h2')).fontSize === saved.font;
        })())");
        require(SUCCEEDED(dashboard->ExecuteScript(L"location.hash='#insights'", nullptr)), "navigate to Insights");
        checkScript(LR"(Boolean(document.querySelector('#coming-page .coming-kicker').textContent === 'A little more biomes, on the way' &&
            document.querySelector('#startup-settings-title').textContent === 'Startup & background' &&
            document.querySelector('#startup-settings-title').classList.contains('privacy-badge')))");
        require(SUCCEEDED(dashboard->ExecuteScript(L"location.hash='#customization'", nullptr)), "return to Customization");
        checkScript(LR"(Boolean(!document.querySelector('#settings-page').hidden &&
            document.querySelector('#startup-settings-title').textContent === 'Startup & background'))");
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
