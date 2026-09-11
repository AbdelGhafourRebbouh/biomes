#include "../../include/core/window_scaler.hpp"
#include "../../include/core/monitor_manager.hpp"
#include "../../include/core/app_launcher.hpp"
#include "../../include/core/launch_progress.hpp"
#include "../../include/core/app_paths.hpp"
#include "../../include/ui/grid_overlay.hpp"
#include "../../include/ui/webview_window.hpp"

#include <iostream>
#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <filesystem>
#include <algorithm>
#include <limits>
#include <fstream>
#include <atomic>
#include <memory>
#include <cmath>

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
    ULONGLONG id = 0;
    SelectedBox box;
    DWORD launchPid = 0;
    string exeName;
    vector<string> expectedAumids;
    unordered_set<HWND> knownWindows;
    ULONGLONG deadline = 0;
    HWND snappedHwnd = nullptr;
    string fullPath;
    bool dispatched = false;
    HWND candidateHwnd = nullptr;
    RECT candidateRect{};
    string candidateTitle;
    ULONGLONG candidateSince = 0;
    DWORD snappedPid = 0;
    bool provisionalWindow = false;
};

struct LaunchResult {
    atomic<bool> done{false};
    atomic<bool> cancelled{false};
    bool success = false;
    DWORD pid = 0;
    string error;
};
struct LaunchJob {
    ULONGLONG id;
    shared_ptr<LaunchResult> result;
};
vector<LaunchJob> g_launchJobs;
struct PlacementCheck {
    HWND hwnd;
    DWORD pid;
    RECT target;
    ULONGLONG due;
    unsigned retries = 0;
    bool waitingRestore = false;
    ULONGLONG restoreDeadline = 0;
    bool refreshNotion = false;
    bool returnFromRefresh = false;
    SelectedBox zone;
    unsigned restoreAttempts = 0;
    unsigned retargets = 0;
};
vector<PlacementCheck> g_placementChecks;
struct LaunchContext {
    SelectedBox box;
    string fullPath;
    shared_ptr<LaunchResult> result;
};
void CALLBACK LaunchWorker(PTP_CALLBACK_INSTANCE, void* parameter);
bool CalculateTargetRect(const SelectedBox& box, RECT& outTarget);

vector<PendingSnap> g_pendingSnaps;
unordered_set<HWND> g_pendingClaimedWindows;
HWINEVENTHOOK g_pendingObjectHook = nullptr;
HWINEVENTHOOK g_pendingForegroundHook = nullptr;
ULONGLONG g_pendingGeneration = 0;
ULONGLONG g_nextPendingId = 0;
UINT_PTR g_pendingTimer = 0;
bool g_processingPending = false;
bool g_trackingPaused = false;
bool g_scanRequested = true;
LaunchProgressState g_launchProgress;
ULONGLONG g_progressDeadline = 0;
std::function<void(const std::string&)> g_progressCallback;
ULONGLONG g_lastScan = 0;

void TrackerLog(const string& message) {
    ofstream log(biomes::AppPaths::RuntimeLog(), ios::app);
    if (log) log << "[TRACKER] " << GetTickCount64() << " " << message << endl;
}

string RectDescription(const RECT& rect) {
    return to_string(rect.left) + "," + to_string(rect.top) + " " +
           to_string(rect.right - rect.left) + "x" + to_string(rect.bottom - rect.top);
}

bool LooksFullscreen(LONG_PTR style, bool minimized, bool maximized,
                     const RECT& rect, const RECT& monitor) {
    // Geometry and style are only hints, not proof of exclusive/F11 fullscreen.
    return !minimized && !maximized && !(style & (WS_CAPTION | WS_THICKFRAME)) &&
        abs(rect.left - monitor.left) <= 2 && abs(rect.top - monitor.top) <= 2 &&
        abs(rect.right - monitor.right) <= 2 && abs(rect.bottom - monitor.bottom) <= 2;
}

string DescribeWindowState(HWND hwnd, const RECT& actual) {
    if (IsIconic(hwnd)) return "minimized";
    if (IsZoomed(hwnd)) return "maximized";
    MONITORINFO monitor{sizeof(MONITORINFO)};
    if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor) &&
        LooksFullscreen(GetWindowLongPtrW(hwnd, GWL_STYLE), false, false, actual, monitor.rcMonitor))
        return "fullscreen-like (unconfirmed)";
    return "normal";
}

bool RequestTargetPlacement(HWND hwnd, const RECT& target, const char* stage, bool show = false) {
    const BOOL accepted = SetWindowPos(hwnd, nullptr, target.left, target.top,
        target.right-target.left, target.bottom-target.top,
        SWP_ASYNCWINDOWPOS | SWP_NOACTIVATE | SWP_NOZORDER | (show ? SWP_SHOWWINDOW : 0));
    if (!accepted) {
        const DWORD error = GetLastError();
        TrackerLog(string("placement API failed stage=") + stage + " hwnd=" +
                   to_string(reinterpret_cast<uintptr_t>(hwnd)) + " error=" + to_string(error) +
                   " target=" + RectDescription(target));
    }
    return accepted != FALSE;
}

struct ScopedFlag {
    bool& value;
    explicit ScopedFlag(bool& flag) : value(flag) { value = true; }
    ~ScopedFlag() { value = false; }
};

void CALLBACK LaunchWorker(PTP_CALLBACK_INSTANCE, void* parameter) {
    unique_ptr<LaunchContext> context(static_cast<LaunchContext*>(parameter));
    auto result = context->result;
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    try {
        if (FAILED(com)) throw runtime_error("COM initialization failed");
        if (!result->cancelled.load()) {
            const auto& box = context->box;
            const auto& fullPath = context->fullPath;
            const bool obsidian = AppLauncher::IsObsidianExe(fullPath);
            const bool packaged = AppLauncher::IsPackagedAppPath(fullPath) || !box.aumid.empty();
            string outError;
    DWORD pid = 0;
    bool launched = false;

    if (obsidian) {
        const string uri = AppLauncher::ResolveObsidianLaunchUri(box);
        if (uri.empty()) {
            outError = "Obsidian vault could not be resolved";
        } else if (!result->cancelled.load()) {
            launched = AppLauncher::LaunchObsidianWithUri(uri, pid);
        }
    } else if (packaged) {
        if (!result->cancelled.load())
            launched = AppLauncher::LaunchPackagedAppForBox(box, pid);
    } else if (FileExists(fullPath)) {
        STARTUPINFOA startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        string commandLine = "\"" + fullPath + "\"";
        vector<char> commandBuffer(commandLine.begin(), commandLine.end());
        commandBuffer.push_back('\0');
        launched = !result->cancelled.load() && CreateProcessA(fullPath.c_str(), commandBuffer.data(), nullptr, nullptr,
                                  FALSE, 0, nullptr, nullptr, &startup, &process) != FALSE;
        if (launched) {
            pid = process.dwProcessId;
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
        } else if (!result->cancelled.load()) {
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

            result->success = launched;
            result->pid = pid;
            result->error = outError.empty() && !launched ? "Windows error " + to_string(GetLastError()) : outError;
        }
    } catch (const exception& error) { result->error = error.what(); }
      catch (...) { result->error = "Unexpected launch failure"; }
    if (SUCCEEDED(com)) CoUninitialize();
    result->done.store(true);
}

bool IsWorkspaceCandidate(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd) || !WindowScaler::IsMainApplicationWindow(hwnd)) return false;
    // Fixed-size application windows are valid; actual dialogs remain choosers.
    wchar_t className[128]{};
    GetClassNameW(hwnd, className, 128);
    return IsWindowEnabled(hwnd) && wcscmp(className, L"#32770") != 0;
}

bool MatchesPendingSnap(const PendingSnap& pending, const WindowInfo& window) {
    for (const auto& aumid : pending.expectedAumids) {
        if (!aumid.empty() && !window.aumid.empty() &&
            _stricmp(aumid.c_str(), window.aumid.c_str()) == 0) {
            return true;
        }
    }
    if (!pending.expectedAumids.empty()) return false;
    // A PID alone can belong to a bootstrapper or be reused. Require executable
    // identity too; when full paths are available, do not match same-name apps
    // from different installations.
    if (!pending.fullPath.empty() && !window.processPath.empty())
        return _stricmp(pending.fullPath.c_str(), window.processPath.c_str()) == 0;
    return !pending.exeName.empty() && !window.processName.empty() &&
           _stricmp(pending.exeName.c_str(), window.processName.c_str()) == 0;
}

int CandidateScore(const PendingSnap& pending, const WindowInfo& window) {
    if (!MatchesPendingSnap(pending, window)) return -1;
    int score = 100;
    if (pending.launchPid && window.processId == pending.launchPid) score += 100;
    if (!pending.box.titleHint.empty() && window.title == pending.box.titleHint) score += 200;
    if (GetWindowLongPtr(window.hwnd, GWL_STYLE) & WS_THICKFRAME) score += 20;
    return score;
}

bool IsStartupCandidate(const WindowInfo& window) {
    // Hints, not exclusions: a legitimate fixed-size application still opens.
    // Only a transition out of this state permits a visible-window handoff.
    if (!(GetWindowLongPtr(window.hwnd, GWL_STYLE) & WS_THICKFRAME)) return true;
    return _stricmp(window.title.c_str(), "Welcome") == 0 ||
           _strnicmp(window.title.c_str(), "Welcome to ", 11) == 0 ||
           _stricmp(window.title.c_str(), "Select Project") == 0 ||
           _stricmp(window.title.c_str(), "Open Project") == 0;
}

void StopPendingHooksIfIdle() {
    if (!g_pendingSnaps.empty() || !g_launchJobs.empty() || !g_placementChecks.empty()) return;
    if (g_pendingTimer) {
        KillTimer(nullptr, g_pendingTimer);
        g_pendingTimer = 0;
    }
    if (g_pendingObjectHook) {
        UnhookWinEvent(g_pendingObjectHook);
        g_pendingObjectHook = nullptr;
    }
    if (g_pendingForegroundHook) {
        UnhookWinEvent(g_pendingForegroundHook);
        g_pendingForegroundHook = nullptr;
    }
}

void ProcessPendingSnaps() {
    if (g_processingPending || g_trackingPaused) return;
    ScopedFlag processing(g_processingPending);
    if (g_launchProgress.active && g_launchProgress.sealed && g_progressDeadline && GetTickCount64() >= g_progressDeadline) {
        bool changed = false;
        for (auto& entry : g_launchProgress.items) {
            if (entry.second.state == "opening" || entry.second.state == "waiting") {
                entry.second.state = "failed";
                entry.second.detail = "Timed out waiting for the app to finish opening.";
                changed = true;
            }
        }
        g_progressDeadline = 0;
        if (changed && g_progressCallback) g_progressCallback(g_launchProgress.Snapshot().dump());
    }
    const auto placementGeneration = g_pendingGeneration;
    // Win32/COM calls can dispatch sent messages. Work on detached storage so
    // cancellation cannot invalidate the verifier's iterators.
    auto checks = std::move(g_placementChecks);
    g_placementChecks.clear();
    for (auto check = checks.begin(); check != checks.end();) {
        if (placementGeneration != g_pendingGeneration) return;
        if (GetTickCount64() < check->due) { ++check; continue; }
        DWORD pid = 0;
        GetWindowThreadProcessId(check->hwnd, &pid);
        RECT actual{};
        if (pid != check->pid || !GetWindowRect(check->hwnd, &actual)) {
            WindowScaler::ReportLaunchState(check->zone, "failed", "Window closed before placement was verified.");
            check = checks.erase(check); continue;
        }
        RECT target{};
        if (!CalculateTargetRect(check->zone, target)) {
            TrackerLog("placement cancelled: monitor unavailable or zone invalid pid=" + to_string(pid));
            WindowScaler::ReportLaunchState(check->zone, "skipped", "Monitor unavailable.");
            check = checks.erase(check); continue;
        }
        if (!EqualRect(&check->target, &target)) {
            if (++check->retargets > 2) {
                TrackerLog("placement cancelled: display bounds keep changing pid=" + to_string(pid));
                WindowScaler::ReportLaunchState(check->zone, "failed", "Display bounds changed repeatedly.");
                WebViewWindow::SendMessageToUI(R"({"action":"STATUS","payload":"Display bounds changed repeatedly during placement. Wait for your display setup to settle, then try the biome again."})");
                check = checks.erase(check); continue;
            }
            TrackerLog("placement target changed pid=" + to_string(pid) + " from=" +
                       RectDescription(check->target) + " to=" + RectDescription(target));
            check->target = target;
            check->retries = 0;
        }
        const bool minimized = IsIconic(check->hwnd) != FALSE;
        const bool maximized = IsZoomed(check->hwnd) != FALSE;
        if (check->waitingRestore || minimized || maximized) {
            if (minimized || maximized) {
                check->waitingRestore = true;
                if (GetTickCount64() >= check->restoreDeadline) {
                    WindowScaler::ReportLaunchState(check->zone, "failed", "Could not restore the app to a normal window.");
                    TrackerLog("restore did not settle; leaving window unchanged pid=" + to_string(pid) +
                               " minimized=" + to_string(minimized) + " maximized=" + to_string(maximized));
                    WebViewWindow::SendMessageToUI(R"({"action":"STATUS","payload":"An app could not return to a normal window and has been left open. Restore it manually and try the biome again."})");
                    check = checks.erase(check); continue;
                }
                // SW_RESTORE can bring a minimized window back maximized. Request
                // the normal state on a later turn instead of waiting forever for
                // that first restore to also unmaximize it. Bound retries and do
                // not alter the session's original WINDOWPLACEMENT or app styles.
                if (check->restoreAttempts < 3) {
                    ++check->restoreAttempts;
                    const BOOL requested = ShowWindowAsync(check->hwnd, SW_SHOWNOACTIVATE);
                    const DWORD error = requested ? ERROR_SUCCESS : GetLastError();
                    if (placementGeneration != g_pendingGeneration) return;
                    TrackerLog("normal restore requested pid=" + to_string(pid) +
                               " attempt=" + to_string(check->restoreAttempts) +
                               " minimized=" + to_string(minimized) + " maximized=" + to_string(maximized) +
                               " error=" + to_string(error));
                }
                check->due = GetTickCount64() + 1000;
                ++check; continue;
            }
            // Restore and resize are separate turns of the target's message loop.
            check->waitingRestore = false;
            RequestTargetPlacement(check->hwnd, target, "after restore", true);
            check->due = GetTickCount64() + 1000;
            ++check; continue;
        }
        if (check->returnFromRefresh) {
            check->returnFromRefresh = false;
            RequestTargetPlacement(check->hwnd, target, "renderer return");
            check->due = GetTickCount64() + 1000;
            ++check; continue;
        }
        const bool fits = abs(actual.left - target.left) <= 16 && abs(actual.top - target.top) <= 16 &&
            abs(actual.right - target.right) <= 16 && abs(actual.bottom - target.bottom) <= 16;
        if (fits) {
            if (check->refreshNotion && target.right-target.left > 2) {
                // One real size transition lets Notion relayout its renderer even
                // when the outer HWND already matches. Never synthesize WM_SIZE
                // with stale dimensions or resize the application's child HWNDs.
                check->refreshNotion = false;
                RECT refreshTarget = target;
                --refreshTarget.right;
                if (RequestTargetPlacement(check->hwnd, refreshTarget, "renderer refresh")) {
                    check->returnFromRefresh = true;
                    check->due = GetTickCount64() + 500;
                    ++check; continue;
                }
            }
            // Schedule painting; do not synchronously call into a hung renderer.
            const auto startup = find_if(g_pendingSnaps.begin(), g_pendingSnaps.end(),
                [&](const PendingSnap& pending) { return pending.snappedHwnd == check->hwnd && pending.provisionalWindow &&
                    (pending.candidateTitle == "Welcome" || pending.candidateTitle.find("Welcome to ") == 0 ||
                     pending.candidateTitle == "Select Project" || pending.candidateTitle == "Open Project"); });
            WindowScaler::ReportLaunchState(check->zone, startup == g_pendingSnaps.end() ? "ready" : "waiting",
                startup == g_pendingSnaps.end() ? "" : "Choose a project in the app.");
            RedrawWindow(check->hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
            RECT client{};
            GetClientRect(check->hwnd, &client);
            TrackerLog("placement settled pid=" + to_string(pid) + " client=" +
                       to_string(client.right) + "x" + to_string(client.bottom) +
                       " actual=" + RectDescription(actual) + " dpi=" + to_string(GetDpiForWindow(check->hwnd)));
            check = checks.erase(check); continue;
        }
        if (++check->retries >= 2) {
            const bool positioned = abs(actual.left - target.left) <= 16 && abs(actual.top - target.top) <= 16;
            const auto startup = find_if(g_pendingSnaps.begin(), g_pendingSnaps.end(),
                [&](const PendingSnap& pending) { return pending.snappedHwnd == check->hwnd && pending.provisionalWindow &&
                    (pending.candidateTitle == "Welcome" || pending.candidateTitle.find("Welcome to ") == 0 ||
                     pending.candidateTitle == "Select Project" || pending.candidateTitle == "Open Project"); });
            WindowScaler::ReportLaunchState(check->zone, startup != g_pendingSnaps.end() ? "waiting" : positioned ? "constrained" : "failed",
                startup != g_pendingSnaps.end() ? "Choose a project in the app." : positioned
                    ? "Opened with the app's size limits." : "App opened, but did not accept its assigned position.");
            // Keep the app open at the size it accepts. Do not repeatedly force
            // its frame, clip its controls, or reposition neighboring windows.
            TrackerLog("placement mismatch; left open hwnd=" + to_string(reinterpret_cast<uintptr_t>(check->hwnd)) +
                " target=" + RectDescription(target) + " actual=" + RectDescription(actual) +
                " state=" + DescribeWindowState(check->hwnd, actual) +
                " dpi=" + to_string(GetDpiForWindow(check->hwnd)));
            check = checks.erase(check); continue;
        }
        RequestTargetPlacement(check->hwnd, target, "verification retry");
        check->due = GetTickCount64() + 1000;
        ++check;
    }
    if (placementGeneration != g_pendingGeneration) return;
    for (const auto& remaining : checks) {
        if (none_of(g_placementChecks.begin(), g_placementChecks.end(),
                    [&](const PlacementCheck& item) { return item.hwnd == remaining.hwnd; }))
            g_placementChecks.push_back(remaining);
    }
    for (auto job = g_launchJobs.begin(); job != g_launchJobs.end();) {
        if (!job->result->done.load()) { ++job; continue; }
        auto pending = find_if(g_pendingSnaps.begin(), g_pendingSnaps.end(),
            [&](const PendingSnap& item) { return item.id == job->id; });
        if (pending != g_pendingSnaps.end()) {
            if (job->result->success) {
                pending->launchPid = job->result->pid;
                pending->deadline = GetTickCount64() + 300000;
                TrackerLog("launch accepted id=" + to_string(job->id) + " pid=" + to_string(pending->launchPid));
            } else {
                TrackerLog("launch failed id=" + to_string(job->id) + " " + job->result->error);
                WindowScaler::ReportLaunchState(pending->box, "failed", "Windows could not launch this app. Check the local log.");
                g_pendingSnaps.erase(pending);
            }
        }
        job = g_launchJobs.erase(job);
    }
    // Work on an ID snapshot: monitor lookup may dispatch callbacks, so no live
    // vector iterator may span it. Drop missing zones before launching or matching.
    const auto monitorSnapshot = g_pendingSnaps;
    for (const auto& pending : monitorSnapshot) {
        RECT target{};
        const bool available = CalculateTargetRect(pending.box, target);
        if (placementGeneration != g_pendingGeneration) return;
        if (available) continue;
        WindowScaler::ReportLaunchState(pending.box, "skipped", "Monitor disconnected.");
        for (const auto& job : g_launchJobs)
            if (job.id == pending.id) job.result->cancelled.store(true);
        g_pendingSnaps.erase(remove_if(g_pendingSnaps.begin(), g_pendingSnaps.end(),
            [&](const PendingSnap& item) { return item.id == pending.id; }), g_pendingSnaps.end());
        TrackerLog("launch/tracking skipped: monitor unavailable id=" + to_string(pending.id));
    }
    for (auto& pending : g_pendingSnaps) {
        if (g_launchJobs.size() >= 3) break;
        if (pending.dispatched) continue;
        auto result = make_shared<LaunchResult>();
        auto context = make_unique<LaunchContext>(LaunchContext{pending.box, pending.fullPath, result});
        g_launchJobs.push_back({pending.id, result});
        if (!TrySubmitThreadpoolCallback(LaunchWorker, context.get(), nullptr)) {
            g_launchJobs.pop_back();
            WindowScaler::ReportLaunchState(pending.box, "failed", "Could not schedule the app launch.");
            pending.dispatched = true;
            pending.deadline = GetTickCount64();
            TrackerLog("worker submission failed " + to_string(GetLastError()));
            break;
        }
        context.release();
        pending.dispatched = true;
    }
    const auto generation = g_pendingGeneration;
    const auto pendingSnapshot = g_pendingSnaps;
    const auto windows = WindowScaler::GetActiveWindows();
    for (const auto& pending : pendingSnapshot) {
        if (generation != g_pendingGeneration) return;
        auto findPending = [&]() {
            return find_if(g_pendingSnaps.begin(), g_pendingSnaps.end(),
                           [&](const PendingSnap& item) { return item.id == pending.id; });
        };
        if (findPending() == g_pendingSnaps.end()) continue;
        if (!pending.dispatched || pending.deadline == 0) continue;
        if (GetTickCount64() >= pending.deadline) {
            const auto progressItem = g_launchProgress.items.find(LaunchProgressState::Key(pending.box));
            if (progressItem != g_launchProgress.items.end() &&
                (progressItem->second.state == "opening" || progressItem->second.state == "waiting"))
                WindowScaler::ReportLaunchState(pending.box, "failed", "Timed out waiting for a workspace window.");
            TrackerLog(string(pending.snappedHwnd ? "tracking completed id=" : "timeout id=") + to_string(pending.id) + " app=" + pending.exeName);
            g_pendingSnaps.erase(findPending());
            continue;
        }
        DWORD currentPid = 0;
        if (pending.snappedHwnd) GetWindowThreadProcessId(pending.snappedHwnd, &currentPid);
        const bool currentAlive = pending.snappedHwnd && currentPid == pending.snappedPid;
        // Never undo a user's minimize, or keep chasing normal document changes.
        if (currentAlive && (IsIconic(pending.snappedHwnd) ||
            (IsWindowVisible(pending.snappedHwnd) && !pending.provisionalWindow))) continue;
        if (pending.snappedHwnd && currentPid != pending.snappedPid)
            g_pendingClaimedWindows.erase(pending.snappedHwnd);
        HWND candidate = nullptr;
        const WindowInfo* candidateInfo = nullptr;
        int bestScore = -1;
        bool ambiguous = false;
        for (const auto& window : windows) {
            const bool ownProvisional = currentAlive && pending.provisionalWindow &&
                                        window.hwnd == pending.snappedHwnd;
            if (pending.knownWindows.count(window.hwnd) ||
                (g_pendingClaimedWindows.count(window.hwnd) && !ownProvisional)) continue;
            if (currentAlive && IsWindowVisible(pending.snappedHwnd) &&
                pending.provisionalWindow && IsStartupCandidate(window)) continue;
            const int score = CandidateScore(pending, window);
            if (score < 0 || !IsWorkspaceCandidate(window.hwnd)) continue;
            if (score > bestScore) {
                bestScore = score;
                candidate = window.hwnd;
                candidateInfo = &window;
                ambiguous = false;
            } else if (score == bestScore) ambiguous = true;
        }
        if (!candidate || ambiguous) {
            auto reset = findPending();
            if (reset != g_pendingSnaps.end()) {
                reset->candidateHwnd = nullptr;
                reset->candidateSince = 0;
            }
            continue;
        }
        auto stable = findPending();
        if (stable == g_pendingSnaps.end()) continue;
        if (stable->candidateHwnd != candidate || stable->candidateTitle != candidateInfo->title ||
            !EqualRect(&stable->candidateRect, &candidateInfo->rect)) {
            stable->candidateHwnd = candidate;
            stable->candidateRect = candidateInfo->rect;
            stable->candidateTitle = candidateInfo->title;
            stable->candidateSince = GetTickCount64();
            continue;
        }
        if (GetTickCount64() - stable->candidateSince < 1000) continue;
        // Never retain a vector iterator/reference across Win32 calls.
        WindowScaler::CacheBiomeAppPreState(candidate, true);
        if (generation != g_pendingGeneration) return;
        const bool placed = WindowScaler::ForceSnapToBox(candidate, pending.box);
        if (generation != g_pendingGeneration) return;
        auto current = findPending();
        if (current == g_pendingSnaps.end()) continue;
        if (placed) {
            g_pendingClaimedWindows.insert(candidate);
            TrackerLog("placement requested id=" + to_string(pending.id) + " app=" + pending.exeName);
            current->snappedHwnd = candidate;
            current->provisionalWindow = IsStartupCandidate(*candidateInfo);
            current->candidateHwnd = nullptr;
            current->candidateSince = 0;
            TrackerLog(string(current->provisionalWindow ? "startup window tracked id=" : "workspace bound id=") +
                       to_string(pending.id));
            // The placement HWND can belong to ApplicationFrameHost, whereas
            // WindowInfo identifies the inner packaged process.
            GetWindowThreadProcessId(candidate, &current->snappedPid);
        }
    }
    StopPendingHooksIfIdle();
}

void CALLBACK PendingTimerProc(HWND, UINT, UINT_PTR timer, DWORD) {
    if (timer != g_pendingTimer) return;
    if (!g_scanRequested && GetTickCount64() - g_lastScan < 2000) return;
    if (g_processingPending || g_trackingPaused) return;
    g_scanRequested = false;
    g_lastScan = GetTickCount64();
    try {
        ProcessPendingSnaps();
    } catch (const exception& error) {
        try { TrackerLog(string("exception: ") + error.what()); } catch (...) {}
        WindowScaler::CancelPendingLaunches();
    } catch (...) {
        WindowScaler::CancelPendingLaunches();
    }
}

void CALLBACK PendingWinEventProc(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD) {
    // Deliberately do not enumerate, place or mutate state in a WinEvent callback.
    // The owner-thread timer coalesces notifications and reconciles missed events.
    g_scanRequested = true;
}

bool EnsurePendingHooks() {
    if (!g_pendingTimer) g_pendingTimer = SetTimer(nullptr, 0, 500, PendingTimerProc);
    if (!g_pendingObjectHook) {
        g_pendingObjectHook = SetWinEventHook(EVENT_OBJECT_DESTROY, EVENT_OBJECT_NAMECHANGE,
                                              nullptr, PendingWinEventProc, 0, 0,
                                              WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    }
    if (!g_pendingForegroundHook) {
        g_pendingForegroundHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
                                                  nullptr, PendingWinEventProc, 0, 0,
                                                  WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    }
    const bool ready = g_pendingTimer && g_pendingObjectHook && g_pendingForegroundHook;
    if (!ready) StopPendingHooksIfIdle();
    return ready;
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

    const LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (exStyle & (WS_EX_TOOLWINDOW | WS_EX_LAYERED)) return false;
    if (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CHILD) return false;
    if (hwnd == GetShellWindow() || hwnd == GetDesktopWindow()) return false;
    wchar_t className[128]{};
    GetClassNameW(hwnd, className, 128);
    for (const auto* shellClass : {L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd", L"Progman", L"WorkerW"})
        if (wcscmp(className, shellClass) == 0) return false;

    BOOL cloaked = FALSE;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) {
        return false;
    }

    if (GetWindowTextLengthW(hwnd) <= 0) return false;
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

        wchar_t title[1024]{};
        GetWindowTextW(hwnd, title, 1024);
        info.title = WideToUtf8(title);
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
    GetWindowThreadProcessId(hwnd, &session.processId);
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

namespace {
bool CalculateTargetRect(const SelectedBox& box, RECT& outTarget) {
    RECT work{};
    if (!MonitorManager::GetWorkAreaForBox(box.monitorIndex, box.monitorDevice, box.stableMonitorId, work))
        return false;
    return WindowScaler::CalculateRelativeRect(work, box.relX, box.relY,
                                               box.relWidth, box.relHeight, outTarget);
}
}

bool WindowScaler::CalculateRelativeRect(const RECT& work, double x, double y,
                                        double width, double height, RECT& target) {
    if (!isfinite(x) || !isfinite(y) || !isfinite(width) || !isfinite(height) ||
        x < 0 || y < 0 || x >= 1 || y >= 1 || width <= 0 || height <= 0 ||
        x + width > 1.0001 || y + height > 1.0001 ||
        work.right <= work.left || work.bottom <= work.top) return false;
    const double workWidth = static_cast<double>(work.right) - work.left;
    const double workHeight = static_cast<double>(work.bottom) - work.top;
    // Round shared edges, rather than widths, so adjacent thirds have no gaps.
    RECT result{
        static_cast<LONG>(work.left + round(x * workWidth)),
        static_cast<LONG>(work.top + round(y * workHeight)),
        static_cast<LONG>(work.left + round(min(1.0, x + width) * workWidth)),
        static_cast<LONG>(work.top + round(min(1.0, y + height) * workHeight))};
    if (result.right <= result.left || result.bottom <= result.top) return false;
    target = result;
    return true;
}

bool WindowScaler::CalculateGridRect(const RECT& work, int rows, int columns,
                                    int startRow, int endRow, int startColumn,
                                    int endColumn, RECT& target) {
    if (rows <= 0 || columns <= 0 || startRow < 0 || startColumn < 0 ||
        endRow <= startRow || endColumn <= startColumn ||
        endRow > rows || endColumn > columns) return false;
    return CalculateRelativeRect(work, static_cast<double>(startColumn) / columns,
        static_cast<double>(startRow) / rows, static_cast<double>(endColumn - startColumn) / columns,
        static_cast<double>(endRow - startRow) / rows, target);
}

bool WindowScaler::ComputeTargetRect(const SelectedBox& box, RECT& outTarget) {
    return CalculateTargetRect(box, outTarget);
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
    const auto generation = g_pendingGeneration;
    if (!hwnd || !IsWindow(hwnd)) return false;
    RECT target{};
    if (!ComputeTargetRect(box, target)) return false;
    const int width = target.right - target.left, height = target.bottom - target.top;
    if (width <= 0 || height <= 0 || !EnsurePendingHooks()) return false;
    ReportLaunchState(box, "opening", "Placing the workspace window.");
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    // Restore standard minimized/maximized state without toggling F11 or Escape.
    // A normal window filling rcWork is not necessarily application fullscreen.
    const bool restoring = IsIconic(hwnd) || IsZoomed(hwnd);
    RECT initial{};
    if (GetWindowRect(hwnd, &initial))
        TrackerLog("placement begin pid=" + to_string(pid) + " state=" + DescribeWindowState(hwnd, initial) +
                   " actual=" + RectDescription(initial) + " target=" + RectDescription(target));
    if (restoring && !ShowWindowAsync(hwnd, SW_RESTORE)) return false;
    if (!restoring && !RequestTargetPlacement(hwnd, target, "initial", true)) {
        return false;
    }
    if (generation != g_pendingGeneration || !IsWindow(hwnd)) return false;
    g_placementChecks.erase(remove_if(g_placementChecks.begin(), g_placementChecks.end(),
        [&](const PlacementCheck& check) { return check.hwnd == hwnd; }), g_placementChecks.end());
    string processPath, processName;
    QueryProcessImage(pid, processPath, processName);
    const bool notion = _stricmp(processName.c_str(), "Notion.exe") == 0;
    if (generation != g_pendingGeneration || !IsWindow(hwnd)) return false;
    g_placementChecks.push_back({hwnd, pid, target, GetTickCount64() + 1000, 0,
                                 restoring, GetTickCount64() + 8000, notion, false, box});
    g_scanRequested = true;
    // This acknowledges a request; the timer verifies its actual applied rectangle.
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

    auto sessions = std::move(s_biomeAppSessions);
    s_biomeAppSessions.clear();
    for (const auto& entry : sessions) {
        HWND hwnd = entry.first;
        const BiomeAppSession& session = entry.second;
        if (!hwnd || !IsWindow(hwnd)) continue;
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid != session.processId) continue;

        if (session.hadPreBiomeState) {
            WINDOWPLACEMENT placement = session.preBiomePlacement;
            placement.length = sizeof(WINDOWPLACEMENT);
            SetWindowPlacement(hwnd, &placement);
            cout << "[SESSION] Restored pre-biome placement for HWND " << hwnd << endl;
        }

        ShowWindow(hwnd, SW_MINIMIZE);
    }

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
    const auto generation = g_pendingGeneration;
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
    pending.id = ++g_nextPendingId;
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
    // Identity resolution and HWND enumeration can dispatch callbacks. Never
    // attach work from a cancelled session to the session that replaced it.
    if (generation != g_pendingGeneration) {
        outError = "workspace launch cancelled during identity resolution";
        return false;
    }
    if (!EnsurePendingHooks()) {
        outError = "could not install workspace window tracker";
        return false;
    }

    pending.fullPath = fullPath;
    pending.deadline = 0; // Starts only after the background launch completes.
    if (generation != g_pendingGeneration) {
        StopPendingHooksIfIdle();
        outError = "workspace launch cancelled before queueing";
        return false;
    }
    g_pendingSnaps.push_back(std::move(pending));
    g_scanRequested = true;
    return true;
}

void WindowScaler::CancelPendingLaunches() {
    if (g_launchProgress.active) {
        g_launchProgress.active = false;
        if (g_progressCallback) g_progressCallback(g_launchProgress.Snapshot().dump());
    }
    ++g_pendingGeneration;
    for (const auto& job : g_launchJobs) job.result->cancelled.store(true);
    g_pendingSnaps.clear();
    g_placementChecks.clear();
    g_pendingClaimedWindows.clear();
    StopPendingHooksIfIdle();
}

void WindowScaler::PauseLaunchTracking(bool paused) {
    g_trackingPaused = paused;
    if (!paused) g_scanRequested = true;
}

void WindowScaler::SetLaunchProgressCallback(std::function<void(const string&)> callback) {
    g_progressCallback = std::move(callback);
}
void WindowScaler::BeginLaunchProgress(const string& name, const vector<SelectedBox>& boxes) {
    g_launchProgress.Begin(name, boxes);
    g_progressDeadline = GetTickCount64() + 300000;
    if (g_progressCallback) g_progressCallback(g_launchProgress.Snapshot().dump());
}
void WindowScaler::FinishLaunchSetup() {
    g_launchProgress.sealed = true;
    if (g_progressCallback) g_progressCallback(g_launchProgress.Snapshot().dump());
}
void WindowScaler::ReportLaunchState(const SelectedBox& box, const string& state, const string& detail) {
    if (g_launchProgress.Update(box, state, detail) && g_progressCallback)
        g_progressCallback(g_launchProgress.Snapshot().dump());
}
