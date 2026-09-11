// Compile the real tracker into this test TU so internal state can be exercised
// without exposing debug APIs in the shipped app. Only monitor lookup and the
// WebView status sink are replaced; no real workspace configuration is loaded.
#include "../src/core/window_scaler.cpp"
#include <stdexcept>

namespace fixture {
RECT work{-12000, -12000, -10080, -10920};
bool connected = true;
bool useMonitorSnapshot = false;
std::vector<MonitorDetail> monitorSnapshot;
bool cancelOnResize = false;
bool minimumSize = false;
int keyMessages = 0;
int checks = 0;
std::vector<std::string> statuses;
std::vector<HWND> owned;

void Require(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_GETMINMAXINFO) {
        // Keep even maximized fixture windows off the user's visible desktop.
        auto* bounds = reinterpret_cast<MINMAXINFO*>(lp);
        bounds->ptMaxPosition = {-12000, -12000};
        bounds->ptMaxSize = {1200, 900};
        return 0;
    }
    if (msg == WM_KEYDOWN || msg == WM_KEYUP) ++keyMessages;
    if (msg == WM_WINDOWPOSCHANGING) {
        auto* position = reinterpret_cast<WINDOWPOS*>(lp);
        if (cancelOnResize && !(position->flags & SWP_NOSIZE)) {
            cancelOnResize = false;
            WindowScaler::CancelPendingLaunches();
        }
        if (minimumSize && !(position->flags & SWP_NOSIZE)) {
            position->cx = (std::max)(position->cx, 420);
            position->cy = (std::max)(position->cy, 320);
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
HWND Window(const wchar_t* title = L"fixture", bool managed = false, bool fixed = false) {
    HWND hwnd = CreateWindowExW(WS_EX_NOACTIVATE | (managed ? 0 : WS_EX_TOOLWINDOW),
        L"BiomesStabilityFixture", title, WS_POPUP | WS_CAPTION | WS_SYSMENU |
        (fixed ? 0 : WS_THICKFRAME | WS_MAXIMIZEBOX),
        -12000, -12000, 600, 450, nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
    Require(hwnd != nullptr, "create fixture window");
    owned.push_back(hwnd);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    return hwnd;
}
SelectedBox Zone(float width = .4f, float height = .4f) {
    SelectedBox box{};
    box.relX = .1f; box.relY = .1f; box.relWidth = width; box.relHeight = height;
    return box;
}
void Tick() {
    for (auto& check : g_placementChecks) check.due = 0;
    ProcessPendingSnaps();
}
void DrainFixtureMessages() {
    // ShowWindowAsync posts work even for these test-owned windows. Production
    // uses its normal message loop; only the isolated test needs to drain it.
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}
void Reset() {
    useMonitorSnapshot = false; monitorSnapshot.clear();
    cancelOnResize = false; minimumSize = false; connected = true;
    WindowScaler::CloseBiomeSession();
    // Tests insert only synthetic completed/blocked job records, never workers.
    g_launchJobs.clear();
    WindowScaler::PauseLaunchTracking(false);
    StopPendingHooksIfIdle();
    for (HWND hwnd : owned) if (IsWindow(hwnd)) DestroyWindow(hwnd);
    owned.clear(); statuses.clear();
    work = {-12000, -12000, -10080, -10920};
}
PendingSnap Pending(const std::string& title = "fixture") {
    PendingSnap pending{};
    pending.id = ++g_nextPendingId;
    pending.box = Zone(); pending.box.titleHint = title;
    QueryProcessImage(GetCurrentProcessId(), pending.fullPath, pending.exeName);
    pending.launchPid = GetCurrentProcessId();
    pending.dispatched = true;
    pending.deadline = GetTickCount64() + 300000;
    return pending;
}
void SettleCandidate() {
    ProcessPendingSnaps();
    for (auto& pending : g_pendingSnaps) pending.candidateSince = GetTickCount64() - 1500;
    ProcessPendingSnaps();
}
}

bool MonitorManager::GetWorkAreaForBox(int index, const std::string& device, const std::string& stable, RECT& out) {
    if (fixture::useMonitorSnapshot) {
        const auto result = ResolveMonitorForBox({stable, device, index}, fixture::monitorSnapshot);
        for (const auto& monitor : fixture::monitorSnapshot)
            if (result.resolvedIndex >= 0 && monitor.index == result.resolvedIndex) {
                out = monitor.rcWork;
                return true;
            }
        return false;
    }
    out = fixture::work;
    return fixture::connected;
}
void WebViewWindow::SendMessageToUI(const std::string& message) { fixture::statuses.push_back(message); }

int main() {
    biomes::AppPaths::InitializeForTests(std::filesystem::temp_directory_path() /
        (L"biomes-stability-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
    using namespace fixture;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    WNDCLASSW wc{}; wc.lpfnWndProc = Proc; wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = L"BiomesStabilityFixture";
    RegisterClassW(&wc);
    try {
        // Physical geometry is independent of logical DPI and supports negative origins.
        for (LONG scale : {1L, 2L, 3L}) {
            work = {-12000, -12000, -12000 + 1280 * scale, -12000 + 720 * scale};
            RECT target{};
            auto box = Zone(.5f, .5f); box.relX = 0; box.relY = 0;
            Require(CalculateTargetRect(box, target), "scaled geometry accepted");
            Require(target.right-target.left == 640*scale && target.bottom-target.top == 360*scale, "physical geometry scales exactly");
        }
        RECT target{};
        auto bad = Zone(); bad.relWidth = std::numeric_limits<float>::quiet_NaN();
        Require(!CalculateTargetRect(bad, target), "NaN rejected");
        bad = Zone(); bad.relX = .9f;
        Require(!CalculateTargetRect(bad, target), "out-of-monitor zone rejected");
        connected = false;
        Require(!CalculateTargetRect(Zone(), target), "disconnected monitor skipped");
        Reset();

        // Odd work-area dimensions must tile without gaps at shared edges.
        const RECT oddWork{-1921, -100, 0, 981};
        LONG previousRight = oddWork.left;
        for (int column = 0; column < 3; ++column) {
            RECT third{};
            Require(WindowScaler::CalculateGridRect(oddWork, 1, 3, 0, 1, column, column + 1, third), "third accepted");
            Require(third.left == previousRight && third.top == oddWork.top && third.bottom == oddWork.bottom,
                    "thirds share edges and respect work area");
            previousRight = third.right;
        }
        Require(previousRight == oddWork.right, "thirds fill work area");
        RECT gridTarget{};
        Require(WindowScaler::CalculateGridRect(oddWork, 2, 2, 1, 2, 1, 2, gridTarget), "quadrant accepted");
        Require(gridTarget.right == oddWork.right && gridTarget.bottom == oddWork.bottom, "quadrant bounded");
        Require(!WindowScaler::CalculateGridRect(oddWork, 0, 2, 0, 1, 0, 1, gridTarget), "zero grid rejected");
        Require(!WindowScaler::CalculateGridRect(oddWork, 2, 2, 0, 3, 0, 1, gridTarget), "invalid grid edge rejected");
        Require(!WindowScaler::CalculateRelativeRect(oddWork, 1, 0, .00001, 1, gridTarget), "start beyond work area rejected");
        HWND filtered = Window(L"Unicode fixture \u00e9", true);
        Require(WindowScaler::IsMainApplicationWindow(filtered), "ordinary window included");
        const LONG_PTR originalStyle = GetWindowLongPtrW(filtered, GWL_EXSTYLE);
        SetWindowLongPtrW(filtered, GWL_EXSTYLE, originalStyle | WS_EX_LAYERED);
        Require(!WindowScaler::IsManagedAppWindow(filtered), "layered window excluded");
        SetWindowLongPtrW(filtered, GWL_EXSTYLE, originalStyle | WS_EX_TOOLWINDOW);
        Require(!WindowScaler::IsManagedAppWindow(filtered), "tool window excluded");
        SetWindowLongPtrW(filtered, GWL_EXSTYLE, originalStyle);
        ShowWindow(filtered, SW_HIDE);
        Require(!WindowScaler::IsManagedAppWindow(filtered), "hidden window excluded");
        Reset();

        for (int count : {1, 8, 24}) {
            for (int repetition = 0; repetition < 4; ++repetition) {
                for (int i = 0; i < count; ++i) Require(WindowScaler::ForceSnapToBox(Window(), Zone()), "placement accepted");
                Tick();
                Require(g_placementChecks.empty(), "all batch placements verified");
                Reset();
            }
        }
        Require(keyMessages == 0, "no injected fullscreen keys");

        HWND hwnd = Window();
        cancelOnResize = true;
        Require(!WindowScaler::ForceSnapToBox(hwnd, Zone()), "reentrant cancellation aborts initial placement");
        Require(g_placementChecks.empty(), "no stale placement after reentrant cancellation");
        Require(WindowScaler::ForceSnapToBox(hwnd, Zone()), "placement after cancellation works");
        // Force the verifier's retry path to reenter cancellation.
        SetWindowPos(hwnd, nullptr, -12000, -12000, 600, 450, SWP_NOACTIVATE | SWP_NOZORDER);
        cancelOnResize = true;
        Tick();
        Require(g_placementChecks.empty(), "verifier cancellation does not invalidate storage or requeue");
        Reset();

        hwnd = Window(); minimumSize = true;
        Require(WindowScaler::ForceSnapToBox(hwnd, Zone(.1f, .1f)), "small zone requested");
        Tick(); Tick();
        Require(IsWindowVisible(hwnd) && !IsIconic(hwnd), "minimum-size app remains open");
        Require(g_placementChecks.empty() && statuses.empty(), "minimum-size retries bounded without error notification");
        Reset();

        hwnd = Window();
        WindowScaler::ForceSnapToBox(hwnd, Zone()); connected = false; Tick();
        Require(g_placementChecks.empty(), "disconnect cancels outstanding geometry checks");
        Reset();

        PendingSnap pending = Pending();
        WindowInfo info{}; info.processPath = pending.fullPath; info.processName = pending.exeName;
        info.processId = pending.launchPid;
        Require(MatchesPendingSnap(pending, info), "matching executable accepted");
        info.processPath = "C:\\different-install\\same.exe";
        Require(!MatchesPendingSnap(pending, info), "same PID or exe cannot override different path");
        pending.expectedAumids = {"Package!One"}; info.aumid = "Package!Two";
        Require(!MatchesPendingSnap(pending, info), "AUMID mismatch is authoritative");
        info.aumid = "Package!One";
        Require(MatchesPendingSnap(pending, info), "exact AUMID accepted");
        Reset();

        HWND first = Window(L"same", true), second = Window(L"same", true);
        g_pendingSnaps.push_back(Pending("same")); SettleCandidate();
        Require(g_pendingSnaps.front().snappedHwnd == nullptr, "ambiguous duplicate windows not stolen");
        SetWindowTextW(second, L"different"); SettleCandidate();
        Require(g_pendingSnaps.front().snappedHwnd == first, "title disambiguates duplicate app windows");
        DestroyWindow(first);
        SetWindowTextW(second, L"same"); SettleCandidate();
        Require(g_pendingSnaps.front().snappedHwnd == second, "replacement workspace follows destroyed window");
        Reset();

        hwnd = Window(L"Welcome", true);
        g_pendingSnaps.push_back(Pending("Welcome")); SettleCandidate();
        Require(g_pendingSnaps.front().provisionalWindow, "welcome remains provisionally tracked");
        SetWindowTextW(hwnd, L"Document - fixture"); SettleCandidate();
        Require(g_pendingSnaps.front().snappedHwnd == hwnd && !g_pendingSnaps.front().provisionalWindow,
                "same HWND welcome-to-workspace transition finalized");
        Tick();
        SetWindowTextW(hwnd, L"Another document"); SettleCandidate();
        Require(g_placementChecks.empty(), "ordinary document title changes do not resnap");
        Reset();

        first = Window(L"Welcome", true);
        g_pendingSnaps.push_back(Pending("Welcome")); SettleCandidate();
        second = Window(L"Workspace", true); SettleCandidate();
        Require(IsWindowVisible(first) && g_pendingSnaps.front().snappedHwnd == second,
                "workspace replaces still-visible startup window");
        Reset();

        hwnd = Window(L"transient", true);
        g_pendingSnaps.push_back(Pending("transient")); ProcessPendingSnaps();
        ShowWindow(hwnd, SW_HIDE); ProcessPendingSnaps();
        Require(g_pendingSnaps.front().candidateHwnd == nullptr, "hidden candidate loses stability credit");
        ShowWindow(hwnd, SW_SHOWNOACTIVATE); ProcessPendingSnaps();
        Require(!g_pendingSnaps.front().snappedHwnd, "reappearing window must settle again");
        SettleCandidate();
        Require(g_pendingSnaps.front().snappedHwnd == hwnd, "reappearing stable workspace placed");
        Reset();

        hwnd = Window(L"Welcome", true);
        g_pendingSnaps.push_back(Pending("Welcome")); SettleCandidate();
        WindowScaler::CancelPendingLaunches();
        SetWindowTextW(hwnd, L"Workspace"); SettleCandidate();
        Require(g_pendingSnaps.empty() && g_placementChecks.empty(), "cancelled welcome cannot hand off later");
        Reset();

        hwnd = Window(L"fixed", true, true);
        Require(IsWorkspaceCandidate(hwnd), "fixed-size application not rejected solely by style");
        g_pendingSnaps.push_back(Pending("fixed")); SettleCandidate();
        Require(g_pendingSnaps.front().snappedHwnd == hwnd && g_pendingSnaps.front().provisionalWindow,
                "fixed-size app opens while remaining eligible for startup handoff");
        SetWindowLongPtrW(hwnd, GWL_STYLE, GetWindowLongPtrW(hwnd, GWL_STYLE) | WS_THICKFRAME);
        SettleCandidate();
        Require(!g_pendingSnaps.front().provisionalWindow, "fixed-size startup becoming resizable finalizes workspace");
        EnableWindow(hwnd, FALSE);
        Require(!IsWorkspaceCandidate(hwnd), "disabled workspace waits for modal interaction");
        Reset();

        HWND chooser = CreateWindowExW(WS_EX_NOACTIVATE, L"#32770", L"Choose project",
            WS_POPUP | WS_CAPTION | WS_VISIBLE, -12000, -12000, 500, 400,
            nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
        Require(chooser != nullptr, "create native chooser fixture"); owned.push_back(chooser);
        Require(!IsWorkspaceCandidate(chooser), "native project dialog is not the workspace");
        Reset();

        hwnd = Window(L"ready", true);
        auto slow = Pending("slow"), ready = Pending("ready");
        slow.deadline = 0; ready.deadline = 0;
        g_pendingSnaps = {slow, ready};
        auto slowResult = std::make_shared<LaunchResult>();
        auto readyResult = std::make_shared<LaunchResult>();
        readyResult->success = true; readyResult->pid = GetCurrentProcessId(); readyResult->done = true;
        g_launchJobs = {{slow.id, slowResult}, {ready.id, readyResult}};
        SettleCandidate();
        Require(g_pendingSnaps[0].deadline == 0, "slow worker remains pending");
        Require(g_pendingSnaps[1].snappedHwnd == hwnd, "ready app places independently of slow launch");
        Reset();

        hwnd = Window();
        Require(WindowScaler::ForceSnapToBox(hwnd, Zone()), "renderer refresh placement accepted");
        g_placementChecks.front().refreshNotion = true;
        Tick(); Require(g_placementChecks.front().returnFromRefresh, "one renderer-size transition scheduled");
        Tick(); Tick();
        Require(g_placementChecks.empty(), "renderer refresh returns to target and terminates");
        Require(keyMessages == 0, "renderer refresh does not inject keys");
        Reset();

        auto stale = std::make_shared<LaunchResult>();
        auto old = Pending(); g_pendingSnaps.push_back(old); g_launchJobs.push_back({old.id, stale});
        WindowScaler::CancelPendingLaunches();
        Require(stale->cancelled.load(), "in-flight launch marked cancelled");
        auto current = Pending(); current.deadline = 0; g_pendingSnaps.push_back(current);
        stale->success = true; stale->pid = 999; stale->done = true; Tick();
        Require(g_pendingSnaps.front().id == current.id && g_pendingSnaps.front().deadline == 0, "old worker result cannot complete new session");
        Reset();

        auto queued = Pending(); queued.dispatched = false; queued.fullPath = "Z:\\does-not-exist.exe";
        g_pendingSnaps.push_back(queued);
        for (int i=0;i<3;++i) g_launchJobs.push_back({++g_nextPendingId, std::make_shared<LaunchResult>()});
        Tick();
        Require(!g_pendingSnaps.front().dispatched && g_launchJobs.size() == 3, "launch concurrency capped at three");
        Reset();

        hwnd = Window();
        WINDOWPLACEMENT before{sizeof(WINDOWPLACEMENT)}, after{sizeof(WINDOWPLACEMENT)};
        GetWindowPlacement(hwnd, &before);
        WindowScaler::CacheBiomeAppPreState(hwnd, false);
        WindowScaler::ForceSnapToBox(hwnd, Zone()); Tick();
        WindowScaler::CloseBiomeSession(); GetWindowPlacement(hwnd, &after);
        Require(EqualRect(&before.rcNormalPosition, &after.rcNormalPosition), "original placement preserved on close");
        Require(IsIconic(hwnd), "session close retains minimize behavior");
        Reset();
        // Exercise real Win32 minimized-from-maximized restoration repeatedly.
        for (int cycle = 0; cycle < 8; ++cycle) {
            hwnd = Window();
            ShowWindow(hwnd, SW_MAXIMIZE);
            Require(IsZoomed(hwnd), "fixture maximized");
            ShowWindow(hwnd, SW_SHOWMINNOACTIVE);
            Require(IsIconic(hwnd), "fixture minimized from maximized");
            WindowScaler::CacheBiomeAppPreState(hwnd, false);
            Require(WindowScaler::ForceSnapToBox(hwnd, Zone()), "minimized-maximized placement accepted");
            for (int step = 0; step < 6 && !g_placementChecks.empty(); ++step) {
                DrainFixtureMessages(); Tick();
            }
            Require(!IsIconic(hwnd) && !IsZoomed(hwnd), "restoration reaches normal window state");
            RECT placed{}, expected{};
            GetWindowRect(hwnd, &placed); CalculateTargetRect(Zone(), expected);
            Require(EqualRect(&placed, &expected), "restored window reaches assigned zone");
            Require(g_placementChecks.empty(), "restore verification terminates");
            Reset();
        }

        hwnd = Window(); ShowWindow(hwnd, SW_MAXIMIZE);
        Require(WindowScaler::ForceSnapToBox(hwnd, Zone()), "restore before cancellation accepted");
        WindowScaler::CancelPendingLaunches();
        DrainFixtureMessages();
        RECT cancelled{}, unchanged{};
        GetWindowRect(hwnd, &cancelled); Tick(); GetWindowRect(hwnd, &unchanged);
        Require(g_placementChecks.empty() && EqualRect(&cancelled, &unchanged), "cancelled restore does not snap later");
        Reset();

        // Exhausted restore attempts must report failure without hiding the app.
        hwnd = Window(); WindowScaler::ForceSnapToBox(hwnd, Zone());
        ShowWindow(hwnd, SW_MAXIMIZE);
        g_placementChecks.front().restoreAttempts = 3;
        g_placementChecks.front().restoreDeadline = 0;
        Tick();
        Require(g_placementChecks.empty() && !statuses.empty(), "restore timeout bounded and reported");
        Require(IsWindowVisible(hwnd) && !IsIconic(hwnd), "restore timeout leaves app open");
        Require(keyMessages == 0, "restore tests never inject fullscreen keys");
        Reset();
        MonitorDetail primary{}, secondary{};
        primary.index = 0; primary.stableId = "EDID:primary";
        primary.deviceName = "DISPLAY1"; primary.rcWork = work; primary.isPrimary = true;
        secondary.index = 1; secondary.stableId = "EDID:secondary";
        secondary.deviceName = "DISPLAY2"; secondary.rcWork = {-10000, -12000, -8080, -10920};
        MonitorBoxRef primaryZone{primary.stableId, primary.deviceName, 0};
        MonitorBoxRef secondaryZone{secondary.stableId, secondary.deviceName, 1};
        Require(MonitorManager::ResolveMonitorForBox(primaryZone, {primary, secondary}).resolvedIndex == 0,
                "primary layout resolves on two screens");
        Require(MonitorManager::ResolveMonitorForBox(secondaryZone, {primary, secondary}).resolvedIndex == 1,
                "secondary layout resolves on two screens");
        auto reusedName = primary; reusedName.deviceName = secondary.deviceName;
        Require(MonitorManager::ResolveMonitorForBox(secondaryZone, {reusedName}).resolvedIndex < 0,
                "missing hardware identity never falls back to reused display name or index");
        auto remaining = secondary; remaining.index = 0; remaining.deviceName = "DISPLAY1"; remaining.isPrimary = true;
        Require(MonitorManager::ResolveMonitorForBox(secondaryZone, {remaining}).resolvedIndex == 0,
                "remaining secondary retains its own zones after renumbering");
        Require(MonitorManager::ResolveMonitorForBox(primaryZone, {remaining}).resolvedIndex < 0,
                "missing primary zones are not merged onto remaining secondary");
        Require(MonitorManager::ResolveMonitorForBox(primaryZone, {}).resolvedIndex < 0, "no monitor skips zone");
        Require(MonitorManager::ResolveMonitorForBox({"", "DISPLAY2", 0}, {primary}).resolvedIndex < 0,
                "legacy missing device does not fall back to index");
        Require(MonitorManager::ResolveMonitorForBox({"", "", 1}, {primary}).resolvedIndex < 0,
                "legacy secondary index skipped on single primary");
        auto duplicate = primary; duplicate.index = 1;
        Require(MonitorManager::ResolveMonitorForBox(primaryZone, {primary, duplicate}).resolvedIndex < 0,
                "ambiguous hardware identity rejected");

        useMonitorSnapshot = true; monitorSnapshot = {primary};
        auto missing = Pending("missing"); missing.dispatched = false;
        missing.box.stableMonitorId = secondary.stableId;
        missing.box.monitorDevice = secondary.deviceName; missing.box.monitorIndex = 1;
        g_pendingSnaps.push_back(missing); Tick();
        Require(g_pendingSnaps.empty() && g_launchJobs.empty(), "disconnected queued zone never launches");
        auto loading = missing; loading.dispatched = true; loading.deadline = 0;
        auto loadingResult = std::make_shared<LaunchResult>();
        g_pendingSnaps.push_back(loading); g_launchJobs.push_back({loading.id, loadingResult}); Tick();
        Require(loadingResult->cancelled.load() && g_pendingSnaps.empty(), "disconnect cancels pending launch tracking");
        loadingResult->success = true; loadingResult->done = true; Tick();
        Require(g_pendingSnaps.empty() && g_placementChecks.empty(), "late disconnected completion cannot place");
        Reset();

        // Step 4: changed work-area geometry gets a bounded fresh placement attempt.
        hwnd = Window(); WindowScaler::ForceSnapToBox(hwnd, Zone());
        g_placementChecks.front().retries = 1;
        work.right -= 320; Tick(); Tick();
        RECT changed{}, expectedChanged{};
        GetWindowRect(hwnd, &changed); CalculateTargetRect(Zone(), expectedChanged);
        Require(EqualRect(&changed, &expectedChanged) && g_placementChecks.empty(), "updated work area verified");
        Reset();
        hwnd = Window(); WindowScaler::ForceSnapToBox(hwnd, Zone());
        for (int change = 0; change < 3; ++change) { work.right -= 100; Tick(); }
        Require(g_placementChecks.empty() && !statuses.empty(), "repeated topology changes bounded");
        Reset();
        RECT full{0, 0, 1920, 1080}, workOnly{0, 0, 1920, 1040};
        Require(LooksFullscreen(WS_POPUP, false, false, full, full), "borderless monitor coverage is fullscreen-like");
        Require(!LooksFullscreen(WS_OVERLAPPEDWINDOW, false, false, full, full), "normal full-sized window not fullscreen");
        Require(!LooksFullscreen(WS_POPUP, false, true, full, full), "maximized state distinguished");
        Require(!LooksFullscreen(WS_POPUP, false, false, workOnly, full), "work-area coverage not fullscreen");
        LaunchProgressState progress;
        auto progressBox = Zone(); progressBox.id = 1; progressBox.assignedApp = "fixture.exe"; progressBox.exeName = "fixture.exe";
        auto otherBox = progressBox; otherBox.id = 2;
        progress.Begin("Test biome", {progressBox, otherBox});
        Require(progress.Snapshot()["state"] == "opening", "progress starts opening");
        progress.Update(progressBox, "ready");
        Require(progress.Snapshot()["ready"] == 1 && progress.Snapshot()["state"] == "opening", "one accepted window not whole completion");
        progress.Update(otherBox, "waiting"); progress.sealed = true;
        Require(progress.Snapshot()["waiting"] == 1 && progress.Snapshot()["state"] == "opening", "chooser keeps progress opening");
        progress.Update(otherBox, "failed", "Timed out");
        Require(progress.Snapshot()["state"] == "partial" && progress.Snapshot()["failed"] == 1, "partial completion retains correct counts");
        progress.Update(otherBox, "skipped");
        Require(progress.Snapshot()["state"] == "success" && progress.Snapshot()["total"] == 1, "disconnected zones excluded from eligible total");
        progress.active = false;
        Require(!progress.Update(progressBox, "failed") && progress.Snapshot()["state"] == "cancelled", "cancelled progress ignores late results");
        const auto oldSession = progress.session;
        progress.Begin("Next biome", {progressBox});
        Require(progress.session > oldSession && progress.Snapshot()["ready"] == 0, "new session resets progress");
        progress.Update(progressBox, "ready");
        Require(progress.Snapshot()["state"] == "opening", "setup must seal before completion");
        progress.sealed = true;
        Require(progress.Snapshot()["state"] == "success", "sealed verified placement completes");
        progress.Update(progressBox, "constrained", "Opened with the app's size limits.");
        Require(progress.Snapshot()["state"] == "success" && progress.Snapshot()["ready"] == 1 &&
            progress.Snapshot()["failed"] == 0, "size constraints complete without launch failure");
        Require(progress.Snapshot()["items"][0]["detail"] == "Opened with the app's size limits.", "size note retained for completion");
        progress.Begin("No monitors", {progressBox}); progress.Update(progressBox, "skipped"); progress.sealed = true;
        Require(progress.Snapshot()["state"] == "partial", "all zones skipped not false success");
        hwnd = Window();
        WindowScaler::BeginLaunchProgress("Placement integration", {progressBox});
        Require(WindowScaler::ForceSnapToBox(hwnd, progressBox), "progress placement requested");
        WindowScaler::FinishLaunchSetup();
        Require(g_launchProgress.Snapshot()["ready"] == 0, "SetWindowPos acceptance not counted ready");
        Tick();
        Require(g_launchProgress.Snapshot()["state"] == "success", "verifier reports ready");
        Reset();
        hwnd = Window(); minimumSize = true;
        progressBox.relWidth = .1f; progressBox.relHeight = .1f;
        WindowScaler::BeginLaunchProgress("Size-limited integration", {progressBox});
        Require(WindowScaler::ForceSnapToBox(hwnd, progressBox), "size-limited placement requested");
        WindowScaler::FinishLaunchSetup(); Tick(); Tick();
        Require(g_launchProgress.Snapshot()["state"] == "success" && g_launchProgress.Snapshot()["failed"] == 0,
            "actual size refusal is informational, not failed");
        Require(g_launchProgress.Snapshot()["items"][0]["state"] == "constrained", "verifier preserves constraint note");
        Require(IsWindow(hwnd) && IsWindowVisible(hwnd), "size-limited app stays open");
        Reset();
        std::cout << "PASS: " << checks << " stability assertions\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n'; Reset(); CoUninitialize(); return 1;
    }
    CoUninitialize(); return 0;
}
