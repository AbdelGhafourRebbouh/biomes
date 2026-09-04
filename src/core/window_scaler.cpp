#include "../../include/core/window_scaler.hpp"
#include "../../include/core/monitor_manager.hpp"
#include "../../include/core/app_launcher.hpp"
#include "../../include/ui/grid_overlay.hpp"

#include <iostream>
#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <filesystem>
#include <algorithm>
#include <limits>

#include <windows.h>
#include <appmodel.h>
#include <psapi.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <propsys.h>
#include <propkey.h>

#pragma comment(lib, "dwmapi.lib")

using namespace std;

unordered_map<HWND, OriginalWindowState> WindowScaler::s_originalPositions;
unordered_set<HWND> WindowScaler::s_cleanSlateMinimized;
unordered_map<HWND, BiomeAppSession> WindowScaler::s_biomeAppSessions;

namespace {

constexpr int kMinMainWindowWidth = 200;
constexpr int kMinMainWindowHeight = 200;

string StripQuotes(string value) {
    while (!value.empty() && (value.front() == '"' || value.front() == '\'')) value.erase(value.begin());
    while (!value.empty() && (value.back() == '"' || value.back() == '\'')) value.pop_back();
    return value;
}

string ExpandEnv(const string& value) {
    char buffer[MAX_PATH * 4];
    const DWORD written = ExpandEnvironmentStringsA(value.c_str(), buffer, static_cast<DWORD>(sizeof(buffer)));
    if (written == 0 || written > sizeof(buffer)) return value;
    return string(buffer);
}

bool FileExists(const string& path) {
    const DWORD attrs = GetFileAttributesA(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

bool QueryProcessImage(DWORD pid, string& outPath, string& outName) {
    outPath.clear();
    outName.clear();
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProcess) return false;

    char path[MAX_PATH];
    DWORD size = MAX_PATH;
    if (QueryFullProcessImageNameA(hProcess, 0, path, &size)) {
        outPath = path;
        outName = filesystem::path(outPath).filename().string();
    }
    CloseHandle(hProcess);
    return !outPath.empty();
}

string WideToUtf8(const wchar_t* value) {
    if (!value || !*value) return "";
    const int size = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return "";
    string out(static_cast<size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, value, -1, out.data(), size, nullptr, nullptr) <= 0) return "";
    out.pop_back();
    return out;
}

bool QueryProcessAumid(DWORD pid, string& outAumid) {
    outAumid.clear();
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;

    UINT32 length = 0;
    const LONG first = GetApplicationUserModelId(process, &length, nullptr);
    if (first != ERROR_INSUFFICIENT_BUFFER || length == 0) {
        CloseHandle(process);
        return false;
    }

    vector<wchar_t> value(length);
    const LONG second = GetApplicationUserModelId(process, &length, value.data());
    CloseHandle(process);
    if (second != ERROR_SUCCESS) return false;

    outAumid = WideToUtf8(value.data());
    return !outAumid.empty();
}

bool QueryWindowAumid(HWND hwnd, string& outAumid) {
    outAumid.clear();
    IPropertyStore* store = nullptr;
    if (FAILED(SHGetPropertyStoreForWindow(hwnd, IID_PPV_ARGS(&store))) || !store) return false;

    PROPVARIANT value{};
    PropVariantInit(&value);
    const HRESULT result = store->GetValue(PKEY_AppUserModel_ID, &value);
    if (SUCCEEDED(result) && value.vt == VT_LPWSTR) {
        outAumid = WideToUtf8(value.pwszVal);
    }
    PropVariantClear(&value);
    store->Release();
    return !outAumid.empty();
}

bool IsApplicationFrameHost(const string& processName) {
    return _stricmp(processName.c_str(), "ApplicationFrameHost.exe") == 0;
}

struct ChildIdentitySearch {
    DWORD hostPid = 0;
    string preferredAumid;
    WindowIdentity identity;
    bool found = false;
};

struct PendingSnap {
    SelectedBox box;
    DWORD launchPid = 0;
    string exeName;
    vector<string> expectedAumids;
    unordered_set<HWND> knownWindows;
    ULONGLONG deadline = 0;
    HWND snappedHwnd = nullptr;
};

vector<PendingSnap> g_pendingSnaps;
unordered_set<HWND> g_pendingClaimedWindows;
HWINEVENTHOOK g_pendingObjectHook = nullptr;
HWINEVENTHOOK g_pendingForegroundHook = nullptr;

bool IsWorkspaceCandidate(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd) || !WindowScaler::IsMainApplicationWindow(hwnd)) return false;

    const LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
    // Fixed-size dialogs and splash windows are not workspace targets. Borderless
    // apps are accepted only when they expose a maximize box.
    return (style & WS_THICKFRAME) != 0 || (style & WS_MAXIMIZEBOX) != 0;
}

bool MatchesPendingSnap(const PendingSnap& pending, const WindowInfo& window) {
    for (const auto& aumid : pending.expectedAumids) {
        if (!aumid.empty() && !window.aumid.empty() &&
            _stricmp(aumid.c_str(), window.aumid.c_str()) == 0) {
            return true;
        }
    }
    return (pending.launchPid != 0 && window.processId == pending.launchPid) ||
           (!pending.exeName.empty() && !window.processName.empty() &&
            _stricmp(pending.exeName.c_str(), window.processName.c_str()) == 0);
}

void StopPendingHooksIfIdle() {
    if (!g_pendingSnaps.empty()) return;
    if (g_pendingObjectHook) {
        UnhookWinEvent(g_pendingObjectHook);
        g_pendingObjectHook = nullptr;
    }
    if (g_pendingForegroundHook) {
        UnhookWinEvent(g_pendingForegroundHook);
        g_pendingForegroundHook = nullptr;
    }
}

void ProcessPendingSnaps(HWND eventHwnd = nullptr) {
    const ULONGLONG now = GetTickCount64();
    const vector<WindowInfo> windows = WindowScaler::GetActiveWindows();

    for (auto it = g_pendingSnaps.begin(); it != g_pendingSnaps.end();) {
        if (now >= it->deadline) {
            cerr << "[LAUNCHER] Timed out waiting for workspace window: " << it->exeName << endl;
            it = g_pendingSnaps.erase(it);
            continue;
        }

        HWND candidateHwnd = nullptr;
        for (const auto& window : windows) {
            if (it->knownWindows.count(window.hwnd) || g_pendingClaimedWindows.count(window.hwnd)) continue;
            if (eventHwnd && window.hwnd != eventHwnd) continue;
            if (!MatchesPendingSnap(*it, window) || !IsWorkspaceCandidate(window.hwnd)) continue;
            candidateHwnd = window.hwnd;
            break;
        }

        if (!candidateHwnd) {
            ++it;
            continue;
        }

        WindowScaler::CacheBiomeAppPreState(candidateHwnd, true);
        if (WindowScaler::ForceSnapToBox(candidateHwnd, it->box)) {
            g_pendingClaimedWindows.insert(candidateHwnd);
            cout << "[LAUNCHER] Deferred workspace snap completed for " << it->exeName << endl;
            it = g_pendingSnaps.erase(it);
        } else {
            cerr << "[LAUNCHER] Deferred workspace snap failed for " << it->exeName << endl;
            ++it;
        }
    }
    StopPendingHooksIfIdle();
}

void CALLBACK PendingWinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG objectId,
                                  LONG childId, DWORD, DWORD) {
    if (objectId != OBJID_WINDOW || childId != CHILDID_SELF || !hwnd) return;
    if (event != EVENT_OBJECT_SHOW && event != EVENT_OBJECT_NAMECHANGE &&
        event != EVENT_SYSTEM_FOREGROUND) {
        return;
    }
    ProcessPendingSnaps(GetAncestor(hwnd, GA_ROOT));
}

bool EnsurePendingHooks() {
    if (!g_pendingObjectHook) {
        g_pendingObjectHook = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_NAMECHANGE,
                                              nullptr, PendingWinEventProc, 0, 0,
                                              WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    }
    if (!g_pendingForegroundHook) {
        g_pendingForegroundHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
                                                  nullptr, PendingWinEventProc, 0, 0,
                                                  WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    }
    return g_pendingObjectHook && g_pendingForegroundHook;
}

BOOL CALLBACK FindPackagedChildWindow(HWND hwnd, LPARAM parameter) {
    auto* search = reinterpret_cast<ChildIdentitySearch*>(parameter);
    if (!IsWindowVisible(hwnd)) return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0 || pid == search->hostPid) return TRUE;

    string aumid;
    if (!QueryProcessAumid(pid, aumid)) return TRUE;
    if (!search->preferredAumid.empty() &&
        _stricmp(search->preferredAumid.c_str(), aumid.c_str()) != 0) {
        return TRUE;
    }

    WindowIdentity candidate;
    candidate.processId = pid;
    candidate.aumid = aumid;
    QueryProcessImage(pid, candidate.processPath, candidate.processName);
    search->identity = std::move(candidate);
    search->found = true;
    return FALSE;
}

int WindowArea(const RECT& r) {
    const LONG w = r.right - r.left;
    const LONG h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return 0;
    return static_cast<int>(w) * static_cast<int>(h);
}

bool RectCoversMonitor(const RECT& windowRect, const RECT& monitorRect, double minCoverage = 0.92) {
    const int monArea = WindowArea(monitorRect);
    if (monArea <= 0) return false;

    RECT overlap{};
    if (!IntersectRect(&overlap, &windowRect, &monitorRect)) return false;

    const double coverage = static_cast<double>(WindowArea(overlap)) / static_cast<double>(monArea);
    if (coverage < minCoverage) return false;

    // Also require window to be roughly monitor-sized (not a small centered dialog).
    const int winArea = WindowArea(windowRect);
    return winArea >= static_cast<int>(monArea * 0.85);
}

bool IsEffectivelyFullscreen(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return false;
    if (IsZoomed(hwnd)) return true;

    const LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
    if (style & WS_MAXIMIZE) return true;

    RECT wr{};
    if (!GetWindowRect(hwnd, &wr)) return false;

    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfo(mon, &mi)) return false;

    return RectCoversMonitor(wr, mi.rcMonitor) || RectCoversMonitor(wr, mi.rcWork);
}

bool RectCloseToTarget(const RECT& actual, const RECT& target, int pad = 48) {
    return abs(actual.left - target.left) <= pad &&
           abs(actual.top - target.top) <= pad &&
           abs((actual.right - actual.left) - (target.right - target.left)) <= pad * 2 &&
           abs((actual.bottom - actual.top) - (target.bottom - target.top)) <= pad * 2;
}

void PumpMessagesMs(int totalMs) {
    const DWORD start = GetTickCount();
    while (static_cast<int>(GetTickCount() - start) < totalMs) {
        MSG msg{};
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                PostQuitMessage(static_cast<int>(msg.wParam));
                return;
            }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        Sleep(20);
    }
}

// Obsidian/Electron F11 fullscreen is NOT IsZoomed — must exit before SetWindowPos.
void ExitFullscreenOrMaximized(HWND hwnd, bool fragile) {
    if (!hwnd || !IsWindow(hwnd)) return;

    if (IsIconic(hwnd)) {
        ShowWindow(hwnd, SW_RESTORE);
        PumpMessagesMs(fragile ? 80 : 40);
    }

    if (IsZoomed(hwnd) || (GetWindowLongPtr(hwnd, GWL_STYLE) & WS_MAXIMIZE)) {
        ShowWindow(hwnd, SW_RESTORE);
        SendMessage(hwnd, WM_SYSCOMMAND, SC_RESTORE, 0);
        PumpMessagesMs(fragile ? 100 : 50);
    }

    if (!IsEffectivelyFullscreen(hwnd)) return;

    cout << "[SCALER] Exiting fullscreen before snap (HWND " << hwnd << ")" << endl;

    // 1) Standard restore
    ShowWindow(hwnd, SW_RESTORE);
    SendMessage(hwnd, WM_SYSCOMMAND, SC_RESTORE, 0);
    PumpMessagesMs(80);

    if (!IsEffectivelyFullscreen(hwnd)) return;

    // 2) Electron/Obsidian often use F11 for exclusive fullscreen
    SetForegroundWindow(hwnd);
    PumpMessagesMs(40);
    PostMessage(hwnd, WM_KEYDOWN, VK_F11, 0x00150001);
    PostMessage(hwnd, WM_KEYUP, VK_F11, 0xC0150001);
    PumpMessagesMs(fragile ? 220 : 120);

    if (!IsEffectivelyFullscreen(hwnd)) return;

    // 3) Escape can leave HTML/document fullscreen inside Chromium hosts
    PostMessage(hwnd, WM_KEYDOWN, VK_ESCAPE, 0x00010001);
    PostMessage(hwnd, WM_KEYUP, VK_ESCAPE, 0xC0010001);
    PumpMessagesMs(120);

    if (!IsEffectivelyFullscreen(hwnd)) return;

    // 4) Last resort: force a non-maximized normal placement slightly inset,
    // then the real zone snap can take over. Avoid SetWindowLongPtr on fragile hosts.
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfo(mon, &mi)) {
        const RECT& w = mi.rcWork;
        const int inset = 80;
        SetWindowPos(
            hwnd,
            HWND_TOP,
            w.left + inset,
            w.top + inset,
            (std::max)(400, static_cast<int>((w.right - w.left) - inset * 2)),
            (std::max)(300, static_cast<int>((w.bottom - w.top) - inset * 2)),
            SWP_SHOWWINDOW | SWP_FRAMECHANGED
        );
        PumpMessagesMs(100);
    }
}

} // namespace

bool WindowScaler::GetProcessImage(DWORD pid, string& outPath, string& outName) {
    return QueryProcessImage(pid, outPath, outName);
}

bool WindowScaler::IsExplorerProcess(const string& exeOrPath) {
    const string name = filesystem::path(exeOrPath).filename().string();
    return _stricmp(name.c_str(), "explorer.exe") == 0;
}

bool WindowScaler::IsOurProcessWindow(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    return pid == GetCurrentProcessId();
}

bool WindowScaler::IsManagedAppWindow(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd) || !IsWindowVisible(hwnd)) return false;
    if (GetWindow(hwnd, GW_OWNER) != nullptr) return false;

    const LONG exStyle = GetWindowLongA(hwnd, GWL_EXSTYLE);
    if (exStyle & WS_EX_TOOLWINDOW) return false;
    if ((exStyle & WS_EX_APPWINDOW) == 0 && GetWindowTextLengthA(hwnd) == 0) return false;

    BOOL cloaked = FALSE;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) {
        return false;
    }

    if (GetWindowTextLengthA(hwnd) <= 0) return false;
    return true;
}

bool WindowScaler::IsMainApplicationWindow(HWND hwnd) {
    if (!IsManagedAppWindow(hwnd)) return false;

    // Minimized windows report a tiny "icon" rect — still valid for reopen reuse.
    if (IsIconic(hwnd)) return true;

    RECT rect{};
    if (!GetWindowRect(hwnd, &rect)) return false;

    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    if (width < kMinMainWindowWidth || height < kMinMainWindowHeight) return false;

    return true;
}

bool WindowScaler::ResolveWindowIdentity(HWND hwnd, WindowIdentity& outIdentity) {
    outIdentity = {};
    HWND placementHwnd = GetAncestor(hwnd, GA_ROOT);
    if (!placementHwnd) placementHwnd = hwnd;
    if (!placementHwnd || !IsWindow(placementHwnd)) return false;

    outIdentity.placementHwnd = placementHwnd;
    DWORD outerPid = 0;
    GetWindowThreadProcessId(placementHwnd, &outerPid);
    QueryProcessImage(outerPid, outIdentity.processPath, outIdentity.processName);
    outIdentity.processId = outerPid;
    outIdentity.isApplicationFrameHost = IsApplicationFrameHost(outIdentity.processName);

    string windowAumid;
    QueryWindowAumid(placementHwnd, windowAumid);

    if (!outIdentity.isApplicationFrameHost) {
        if (!QueryProcessAumid(outerPid, outIdentity.aumid)) {
            outIdentity.aumid = windowAumid;
        }
        return true;
    }

    ChildIdentitySearch childSearch;
    childSearch.hostPid = outerPid;
    childSearch.preferredAumid = windowAumid;
    EnumChildWindows(placementHwnd, FindPackagedChildWindow,
                     reinterpret_cast<LPARAM>(&childSearch));
    if (childSearch.found) {
        outIdentity.processId = childSearch.identity.processId;
        outIdentity.processPath = childSearch.identity.processPath;
        outIdentity.processName = childSearch.identity.processName;
        outIdentity.aumid = childSearch.identity.aumid;
        return true;
    }

    // Some UWP frames expose their AUMID only on the outer window. It is still
    // a valid package identity, even when no child process window is enumerable.
    if (!windowAumid.empty()) {
        outIdentity.aumid = windowAumid;
        outIdentity.processId = 0;
        outIdentity.processPath.clear();
        outIdentity.processName.clear();
        return true;
    }

    cerr << "[UWP] Ignoring unresolved ApplicationFrameHost HWND " << placementHwnd << endl;
    return false;
}

vector<WindowInfo> WindowScaler::GetActiveWindows() {
    vector<WindowInfo> windows;
    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        if (!WindowScaler::IsMainApplicationWindow(hwnd)) return TRUE;

        auto* out = reinterpret_cast<vector<WindowInfo>*>(lParam);
        WindowIdentity identity;
        if (!WindowScaler::ResolveWindowIdentity(hwnd, identity)) return TRUE;

        WindowInfo info;
        info.hwnd = identity.placementHwnd;

        char title[512];
        GetWindowTextA(hwnd, title, sizeof(title));
        info.title = title;
        GetWindowRect(hwnd, &info.rect);
        info.processId = identity.processId;
        info.processPath = identity.processPath;
        info.processName = identity.processName;
        info.aumid = identity.aumid;
        out->push_back(info);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&windows));
    return windows;
}

void WindowScaler::CacheOriginalPosition(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return;
    if (s_originalPositions.count(hwnd)) return;

    OriginalWindowState state;
    state.placement.length = sizeof(WINDOWPLACEMENT);
    if (!GetWindowPlacement(hwnd, &state.placement)) {
        cerr << "[MEMORY] GetWindowPlacement failed for HWND " << hwnd << endl;
        return;
    }

    s_originalPositions[hwnd] = state;
}

void WindowScaler::CacheBiomeAppPreState(HWND hwnd, bool launchedFresh) {
    if (!hwnd || !IsWindow(hwnd)) return;
    if (s_biomeAppSessions.count(hwnd)) return;

    BiomeAppSession session;
    session.hwnd = hwnd;
    session.hadPreBiomeState = !launchedFresh;

    if (!launchedFresh) {
        session.preBiomePlacement.length = sizeof(WINDOWPLACEMENT);
        if (GetWindowPlacement(hwnd, &session.preBiomePlacement)) {
            cout << "[SESSION] Cached pre-biome state for HWND " << hwnd << endl;
        } else {
            session.hadPreBiomeState = false;
        }
    }

    s_biomeAppSessions[hwnd] = session;
}

bool WindowScaler::ComputeTargetRect(const SelectedBox& box, RECT& outTarget) {
    RECT work{};
    if (!MonitorManager::GetWorkAreaForBox(box.monitorIndex, box.monitorDevice, box.stableMonitorId, work)) {
        cerr << "[SCALER] Monitor unavailable for zone " << box.id << endl;
        return false;
    }

    const int monWidth = work.right - work.left;
    const int monHeight = work.bottom - work.top;

    outTarget.left = work.left + static_cast<int>(box.relX * monWidth);
    outTarget.top = work.top + static_cast<int>(box.relY * monHeight);
    outTarget.right = outTarget.left + static_cast<int>(box.relWidth * monWidth);
    outTarget.bottom = outTarget.top + static_cast<int>(box.relHeight * monHeight);
    return true;
}

bool WindowScaler::ApplyPlacementRect(HWND hwnd, const RECT& screenRect) {
    if (IsIconic(hwnd) || IsZoomed(hwnd)) {
        ShowWindow(hwnd, SW_RESTORE);
        Sleep(30);
    }

    const int width = screenRect.right - screenRect.left;
    const int height = screenRect.bottom - screenRect.top;

    const BOOL ok = SetWindowPos(
        hwnd,
        HWND_TOP,
        screenRect.left,
        screenRect.top,
        width,
        height,
        SWP_SHOWWINDOW | SWP_FRAMECHANGED
    );

    if (!ok) {
        cerr << "[SCALER] SetWindowPos failed (" << GetLastError() << ")" << endl;
        return false;
    }

    ShowWindow(hwnd, SW_SHOW);
    return true;
}

bool WindowScaler::ForceSnapToBox(HWND hwnd, const SelectedBox& box) {
    if (!hwnd || !IsWindow(hwnd)) return false;

    RECT target{};
    if (!ComputeTargetRect(box, target)) return false;

    const int width = target.right - target.left;
    const int height = target.bottom - target.top;
    if (width <= 0 || height <= 0) return false;

    string processName;
    string processPath;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    QueryProcessImage(pid, processPath, processName);
    const bool fragile = AppLauncher::IsFragileElectronHost(processName) ||
                         AppLauncher::IsFragileElectronHost(box.exeName) ||
                         AppLauncher::IsFragileElectronHost(box.assignedApp);

    // Obsidian F11 / maximized / borderless fullscreen must exit before SetWindowPos.
    ExitFullscreenOrMaximized(hwnd, fragile);

    if (!fragile) {
        LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
        if (style & WS_MAXIMIZE) {
            SetWindowLongPtr(hwnd, GWL_STYLE, style & ~WS_MAXIMIZE);
            SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        }
    }

    auto placeOnce = [&](UINT flags) -> bool {
        return SetWindowPos(
                   hwnd,
                   HWND_TOP,
                   target.left,
                   target.top,
                   width,
                   height,
                   flags) != FALSE;
    };

    // First attempt: show without forcing activation (safer for Notion).
    UINT flags = SWP_SHOWWINDOW | SWP_FRAMECHANGED | SWP_NOCOPYBITS;
    if (fragile && !IsEffectivelyFullscreen(hwnd)) {
        flags |= SWP_NOACTIVATE;
    }

    if (!placeOnce(flags)) {
        cerr << "[SCALER] SetWindowPos failed (" << GetLastError() << ")" << endl;
        if (!ApplyPlacementRect(hwnd, target)) return false;
    }

    PumpMessagesMs(fragile ? 60 : 30);
    placeOnce(SWP_SHOWWINDOW | SWP_FRAMECHANGED);

    RECT after{};
    GetWindowRect(hwnd, &after);
    if (!RectCloseToTarget(after, target) || IsEffectivelyFullscreen(hwnd)) {
        cout << "[SCALER] Snap miss — retry after forcing windowed mode" << endl;
        ExitFullscreenOrMaximized(hwnd, fragile);
        placeOnce(SWP_SHOWWINDOW | SWP_FRAMECHANGED);
        PumpMessagesMs(80);
        placeOnce(SWP_SHOWWINDOW | SWP_FRAMECHANGED);
        GetWindowRect(hwnd, &after);
    }

    HMONITOR expected = MonitorFromRect(&target, MONITOR_DEFAULTTONEAREST);
    HMONITOR actual = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (expected && actual && expected != actual) {
        cout << "[SCALER] Retry snap — window landed on wrong monitor" << endl;
        ExitFullscreenOrMaximized(hwnd, fragile);
        placeOnce(SWP_SHOWWINDOW | SWP_FRAMECHANGED);
    }

    cout << "[SCALER] ForceSnap HWND " << hwnd
         << (fragile ? " (electron)" : "")
         << " mon=" << box.monitorIndex
         << " (" << box.monitorDevice << ")"
         << " -> LTRB "
         << target.left << "," << target.top << "," << target.right << "," << target.bottom
         << endl;
    return true;
}

void WindowScaler::PrepareForOverlayCreate(HWND dashboardHwnd) {
    cout << "[CLEAN] PrepareForOverlayCreate — minimizing managed apps only..." << endl;
    const auto windows = GetActiveWindows();
    for (const auto& win : windows) {
        if (win.hwnd == dashboardHwnd) continue;
        if (IsOurProcessWindow(win.hwnd)) continue;
        if (IsIconic(win.hwnd)) continue;
        ShowWindow(win.hwnd, SW_MINIMIZE);
    }
}

void WindowScaler::MinimizeExeSiblings(const string& exeName, HWND keepHwnd) {
    if (exeName.empty()) return;
    for (const auto& win : GetActiveWindows()) {
        if (win.hwnd == keepHwnd) continue;
        if (_stricmp(win.processName.c_str(), exeName.c_str()) != 0) continue;
        CacheOriginalPosition(win.hwnd);
        ShowWindow(win.hwnd, SW_MINIMIZE);
        s_cleanSlateMinimized.insert(win.hwnd);
        cout << "[CLEAN] Minimized sibling " << exeName << " HWND " << win.hwnd << endl;
    }
}

void WindowScaler::MinimizeExceptPlaced(const vector<HWND>& placedHwnds, HWND dashboardHwnd) {
    unordered_set<HWND> keep(placedHwnds.begin(), placedHwnds.end());
    for (const auto& win : GetActiveWindows()) {
        if (keep.count(win.hwnd)) continue;
        if (dashboardHwnd && win.hwnd == dashboardHwnd) continue;
        if (IsOurProcessWindow(win.hwnd)) continue;
        if (IsIconic(win.hwnd)) continue;

        CacheOriginalPosition(win.hwnd);
        ShowWindow(win.hwnd, SW_MINIMIZE);
        s_cleanSlateMinimized.insert(win.hwnd);
    }
}

void WindowScaler::PrepareCleanSlate(HWND dashboardHwnd, const unordered_set<HWND>& keepVisible) {
    cout << "[CLEAN] Preparing tracked clean slate..." << endl;
    const auto windows = GetActiveWindows();
    for (const auto& win : windows) {
        if (dashboardHwnd && win.hwnd == dashboardHwnd) continue;
        if (IsOurProcessWindow(win.hwnd)) continue;
        if (keepVisible.count(win.hwnd)) continue;
        if (IsIconic(win.hwnd)) continue;

        CacheOriginalPosition(win.hwnd);
        ShowWindow(win.hwnd, SW_MINIMIZE);
        s_cleanSlateMinimized.insert(win.hwnd);
    }
}

void WindowScaler::CloseBiomeSession() {
    CancelPendingLaunches();
    cout << "[SESSION] Closing biome session (" << s_biomeAppSessions.size() << " apps)..." << endl;

    for (const auto& entry : s_biomeAppSessions) {
        HWND hwnd = entry.first;
        const BiomeAppSession& session = entry.second;
        if (!hwnd || !IsWindow(hwnd)) continue;

        if (session.hadPreBiomeState) {
            WINDOWPLACEMENT placement = session.preBiomePlacement;
            placement.length = sizeof(WINDOWPLACEMENT);
            SetWindowPlacement(hwnd, &placement);
            cout << "[SESSION] Restored pre-biome placement for HWND " << hwnd << endl;
        }

        ShowWindow(hwnd, SW_MINIMIZE);
    }

    s_biomeAppSessions.clear();
    // Intentionally leave s_cleanSlateMinimized untouched — non-biome apps stay minimized.
}

void WindowScaler::RaiseBiomeWindows(const vector<HWND>& biomeHwnds) {
    // Do not use HWND_TOPMOST — it can freeze Electron apps (Notion) and steal focus badly.
    for (HWND hwnd : biomeHwnds) {
        if (!hwnd || !IsWindow(hwnd)) continue;
        SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW | SWP_NOACTIVATE);
    }
    if (!biomeHwnds.empty()) {
        HWND last = biomeHwnds.back();
        if (last && IsWindow(last)) {
            AllowSetForegroundWindow(ASFW_ANY);
            SetForegroundWindow(last);
        }
    }
}

string WindowScaler::ResolveAppPath(const string& processNameOrPath) {
    if (processNameOrPath.empty()) return "";

    string candidate = ExpandEnv(StripQuotes(processNameOrPath));
    if (candidate.find('\\') != string::npos || candidate.find('/') != string::npos) {
        if (FileExists(candidate)) return candidate;
    }

    const string exeName = filesystem::path(candidate).filename().string();
    const string subKey = "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\" + exeName;
    for (const HKEY root : { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE }) {
        HKEY hKey = nullptr;
        if (RegOpenKeyExA(root, subKey.c_str(), 0, KEY_READ, &hKey) != ERROR_SUCCESS) continue;

        char pathBuffer[MAX_PATH];
        DWORD bufferSize = sizeof(pathBuffer);
        const LONG result = RegQueryValueExA(hKey, nullptr, nullptr, nullptr,
                                             reinterpret_cast<LPBYTE>(pathBuffer), &bufferSize);
        RegCloseKey(hKey);
        if (result == ERROR_SUCCESS) {
            string resolved = ExpandEnv(StripQuotes(pathBuffer));
            if (FileExists(resolved)) return resolved;
        }
    }

    char pathBuffer[MAX_PATH];
    if (SearchPathA(nullptr, exeName.c_str(), nullptr, MAX_PATH, pathBuffer, nullptr) > 0) {
        return string(pathBuffer);
    }

    return candidate;
}

bool WindowScaler::IsUnsupportedUwpBinding(const string& assignedApp) {
    const string name = filesystem::path(assignedApp).filename().string();
    return _stricmp(name.c_str(), "ApplicationFrameHost.exe") == 0;
}

HWND WindowScaler::WaitForNewWindow(DWORD pid,
                                    const string& exeName,
                                    const unordered_set<HWND>& excludeHwnds,
                                    const vector<string>& expectedAumids,
                                    int timeoutMs) {
    const int stepMs = 200;
    const int attempts = std::max(1, timeoutMs / stepMs);

    HWND best = nullptr;
    int bestArea = 0;

    for (int attempt = 0; attempt < attempts; ++attempt) {
        // Keep the Biomes UI thread alive so the window does not freeze / ding on taskbar click.
        MSG msg{};
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                PostQuitMessage(static_cast<int>(msg.wParam));
                return best;
            }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        Sleep(stepMs);

        for (const auto& win : GetActiveWindows()) {
            if (excludeHwnds.count(win.hwnd)) continue;

            bool aumidMatch = false;
            for (const auto& expectedAumid : expectedAumids) {
                if (!expectedAumid.empty() && !win.aumid.empty() &&
                    _stricmp(win.aumid.c_str(), expectedAumid.c_str()) == 0) {
                    aumidMatch = true;
                    break;
                }
            }
            bool pidMatch = (pid != 0 && win.processId == pid);
            bool exeMatch = (!exeName.empty() && _stricmp(win.processName.c_str(), exeName.c_str()) == 0);
            if (!expectedAumids.empty()) {
                if (!aumidMatch) continue;
            } else if (!pidMatch && !exeMatch) {
                continue;
            }

            const int area = WindowArea(win.rect);
            if (area > bestArea) {
                bestArea = area;
                best = win.hwnd;
            }
        }
        if (best) return best;
    }
    return nullptr;
}

HWND WindowScaler::LaunchAndSnapApp(const string& assignedApp,
                                    const SelectedBox& box,
                                    const unordered_set<HWND>& excludeHwnds,
                                    int waitTimeoutMs) {
    if (IsUnsupportedUwpBinding(assignedApp)) {
        cerr << "[LAUNCHER] Refusing ApplicationFrameHost.exe" << endl;
        return nullptr;
    }

    const string fullPath = ResolveAppPath(assignedApp);
    const string exeName = filesystem::path(fullPath).filename().string();
    cout << "[LAUNCHER] Launching: " << fullPath << endl;

    const bool packagedPath = AppLauncher::IsPackagedAppPath(fullPath);
    const bool obsidian = AppLauncher::IsObsidianExe(exeName) ||
                          AppLauncher::IsObsidianExe(box.assignedApp);

    // Obsidian: never bare exe (vault picker). Resolve URI from title + obsidian.json.
    if (obsidian) {
        for (const auto& win : GetActiveWindows()) {
            if (_stricmp(win.processName.c_str(), "Obsidian.exe") == 0) {
                cerr << "[LAUNCHER] Obsidian already running — reuse existing window" << endl;
                return nullptr;
            }
        }
        const string launchUri = AppLauncher::ResolveObsidianLaunchUri(box);
        if (launchUri.empty()) {
            cerr << "[LAUNCHER] Obsidian requires a resolvable vault — open vault and recreate zone" << endl;
            return nullptr;
        }
        DWORD pid = 0;
        if (!AppLauncher::LaunchObsidianWithUri(launchUri, pid)) {
            return nullptr;
        }
        unordered_set<HWND> knownBefore = excludeHwnds;
        for (const auto& win : GetActiveWindows()) {
            knownBefore.insert(win.hwnd);
        }
        HWND hwnd = WaitForNewWindow(pid, "Obsidian.exe", knownBefore, {}, waitTimeoutMs);
        if (!hwnd) {
            // Protocol launch may reuse another process — accept any new Obsidian window.
            hwnd = WaitForNewWindow(0, "Obsidian.exe", knownBefore, {}, waitTimeoutMs);
        }
        if (!hwnd) {
            cerr << "[LAUNCHER] Timed out waiting for Obsidian after URI launch" << endl;
            return nullptr;
        }
        CacheBiomeAppPreState(hwnd, true);
        if (!ForceSnapToBox(hwnd, box)) return nullptr;
        return hwnd;
    }

    // Store / packaged apps: AUMID activation only — never CreateProcess on WindowsApps path.
    if (AppLauncher::IsPackagedAppPath(fullPath) || AppLauncher::IsPackagedAppPath(box.assignedApp) ||
        !box.aumid.empty()) {
        const auto candidates = AppLauncher::ResolveAumidCandidates(box);
        if (candidates.empty()) {
            cerr << "[LAUNCHER] Packaged app without AUMID — recreate zone while app is open" << endl;
            return nullptr;
        }
        DWORD pid = 0;
        if (!AppLauncher::LaunchPackagedAppForBox(box, pid)) {
            return nullptr;
        }
        unordered_set<HWND> knownBefore = excludeHwnds;
        for (const auto& win : GetActiveWindows()) {
            knownBefore.insert(win.hwnd);
        }
        HWND hwnd = WaitForNewWindow(pid, exeName, knownBefore, candidates, waitTimeoutMs);
        if (!hwnd) {
            hwnd = WaitForNewWindow(0, exeName, knownBefore, candidates, waitTimeoutMs);
        }
        if (!hwnd) {
            cerr << "[LAUNCHER] Timed out waiting for packaged app " << candidates.front() << endl;
            return nullptr;
        }
        CacheBiomeAppPreState(hwnd, true);
        if (!ForceSnapToBox(hwnd, box)) return nullptr;
        return hwnd;
    }

    if (!FileExists(fullPath)) {
        cerr << "[LAUNCHER] Path does not exist: " << fullPath << endl;
        return nullptr;
    }

    if (IsExplorerProcess(exeName)) {
        MinimizeExeSiblings(exeName, nullptr);
    }

    unordered_set<HWND> knownBefore = excludeHwnds;
    for (const auto& win : GetActiveWindows()) {
        knownBefore.insert(win.hwnd);
    }

    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    string commandLine = "\"" + fullPath + "\"";
    vector<char> cmdBuf(commandLine.begin(), commandLine.end());
    cmdBuf.push_back('\0');

    const BOOL created = CreateProcessA(
        fullPath.c_str(),
        cmdBuf.data(),
        nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi
    );

    DWORD launchedPid = 0;
    if (created) {
        launchedPid = pi.dwProcessId;
        WaitForInputIdle(pi.hProcess, 5000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        // Do not ShellExecute packaged paths — shows blocking modal error dialog.
        if (packagedPath) {
            cerr << "[LAUNCHER] Refusing ShellExecute on packaged path" << endl;
            return nullptr;
        }
        SHELLEXECUTEINFOA sei = { sizeof(sei) };
        sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
        sei.lpVerb = "open";
        sei.lpFile = fullPath.c_str();
        sei.nShow = SW_SHOWNORMAL;
        if (!ShellExecuteExA(&sei)) {
            cerr << "[LAUNCHER] ShellExecuteEx failed (" << GetLastError() << ")" << endl;
            return nullptr;
        }
        if (sei.hProcess) {
            launchedPid = GetProcessId(sei.hProcess);
            WaitForInputIdle(sei.hProcess, 5000);
            CloseHandle(sei.hProcess);
        }
    }

    HWND hwnd = WaitForNewWindow(launchedPid, exeName, knownBefore, {}, waitTimeoutMs);
    if (!hwnd) {
        cerr << "[LAUNCHER] Timed out waiting for a new window from " << fullPath << endl;
        return nullptr;
    }

    cout << "[LAUNCHER] New HWND " << hwnd << " owned for snap" << endl;
    CacheBiomeAppPreState(hwnd, true);
    if (!ForceSnapToBox(hwnd, box)) return nullptr;
    return hwnd;
}

bool WindowScaler::LaunchAndTrackApp(const string& assignedApp,
                                     const SelectedBox& box,
                                     const unordered_set<HWND>& excludeHwnds,
                                     string& outError) {
    outError.clear();
    if (IsUnsupportedUwpBinding(assignedApp)) {
        outError = "ApplicationFrameHost.exe is not a launchable app identity";
        return false;
    }

    const string fullPath = ResolveAppPath(assignedApp);
    const string exeName = filesystem::path(fullPath).filename().string();
    const bool packaged = AppLauncher::IsPackagedAppPath(fullPath) ||
                          AppLauncher::IsPackagedAppPath(box.assignedApp) || !box.aumid.empty();
    const bool obsidian = AppLauncher::IsObsidianExe(exeName) ||
                          AppLauncher::IsObsidianExe(box.assignedApp);

    PendingSnap pending;
    pending.box = box;
    pending.exeName = exeName;
    pending.knownWindows = excludeHwnds;
    for (const auto& window : GetActiveWindows()) pending.knownWindows.insert(window.hwnd);
    pending.deadline = GetTickCount64() + 60000;
    if (packaged) pending.expectedAumids = AppLauncher::ResolveAumidCandidates(box);

    if (packaged && pending.expectedAumids.empty()) {
        outError = "Store app has no resolvable AUMID";
        return false;
    }
    if (!EnsurePendingHooks()) {
        outError = "could not install workspace window tracker";
        return false;
    }

    g_pendingSnaps.push_back(std::move(pending));
    PendingSnap& queued = g_pendingSnaps.back();
    DWORD pid = 0;
    bool launched = false;

    if (obsidian) {
        const string uri = AppLauncher::ResolveObsidianLaunchUri(box);
        if (uri.empty()) {
            outError = "Obsidian vault could not be resolved";
        } else {
            launched = AppLauncher::LaunchObsidianWithUri(uri, pid);
        }
    } else if (packaged) {
        launched = AppLauncher::LaunchPackagedAppForBox(box, pid);
    } else if (FileExists(fullPath)) {
        STARTUPINFOA startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        string commandLine = "\"" + fullPath + "\"";
        vector<char> commandBuffer(commandLine.begin(), commandLine.end());
        commandBuffer.push_back('\0');
        launched = CreateProcessA(fullPath.c_str(), commandBuffer.data(), nullptr, nullptr,
                                  FALSE, 0, nullptr, nullptr, &startup, &process) != FALSE;
        if (launched) {
            pid = process.dwProcessId;
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
        } else {
            SHELLEXECUTEINFOA shell{};
            shell.cbSize = sizeof(shell);
            shell.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
            shell.lpVerb = "open";
            shell.lpFile = fullPath.c_str();
            shell.nShow = SW_SHOWNORMAL;
            launched = ShellExecuteExA(&shell) != FALSE;
            if (launched && shell.hProcess) {
                pid = GetProcessId(shell.hProcess);
                CloseHandle(shell.hProcess);
            }
        }
    } else {
        outError = "application path does not exist";
    }

    if (!launched) {
        if (outError.empty()) outError = "launch trigger failed (" + to_string(GetLastError()) + ")";
        g_pendingSnaps.pop_back();
        StopPendingHooksIfIdle();
        return false;
    }

    queued.launchPid = pid;
    cout << "[LAUNCHER] Started async workspace tracking for " << exeName
         << " (PID " << pid << ")" << endl;
    ProcessPendingSnaps();
    return true;
}

void WindowScaler::CancelPendingLaunches() {
    g_pendingSnaps.clear();
    g_pendingClaimedWindows.clear();
    StopPendingHooksIfIdle();
}
