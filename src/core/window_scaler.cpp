#include "../../include/core/window_scaler.hpp"
#include "../../include/ui/grid_overlay.hpp"

#include <iostream>
#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <filesystem>
#include <algorithm>

#include <windows.h>
#include <psapi.h>
#include <shellapi.h>
#include <dwmapi.h>

#pragma comment(lib, "dwmapi.lib")

// ---------------------------------------------------------------------------
// WindowScaler — Win32 placement engine for Biomes
//
// Invariants:
// 1. Never use GetWindowRect while a window is iconic for restore data.
//    Cache WINDOWPLACEMENT before minimize/snap; restore with SetWindowPlacement.
// 2. Do not fake Win+D. Clean slate minimizes non-biome windows and tracks them.
// 3. Launch ownership: only snap a NEW hwnd for the launched PID / exe
//    (never steal an already-assigned Chrome/Electron window).
// ---------------------------------------------------------------------------

using namespace std;

unordered_map<HWND, OriginalWindowState> WindowScaler::s_originalPositions;
unordered_set<HWND> WindowScaler::s_cleanSlateMinimized;

namespace {

struct MonitorEnumContext {
    vector<RECT> rects;
};

BOOL CALLBACK MonitorEnumProc(HMONITOR hMonitor, HDC, LPRECT lprcMonitor, LPARAM dwData) {
    auto* ctx = reinterpret_cast<MonitorEnumContext*>(dwData);
    MONITORINFO mi = { sizeof(MONITORINFO) };
    if (GetMonitorInfoA(hMonitor, &mi)) {
        ctx->rects.push_back(mi.rcWork);
    } else if (lprcMonitor) {
        ctx->rects.push_back(*lprcMonitor);
    }
    return TRUE;
}

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

} // namespace

bool WindowScaler::GetProcessImage(DWORD pid, string& outPath, string& outName) {
    return QueryProcessImage(pid, outPath, outName);
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

    // Require a title for matching — matches Explorer taskbar-ish behavior for V1.
    if (GetWindowTextLengthA(hwnd) <= 0) return false;
    return true;
}

vector<RECT> WindowScaler::GetWorkAreaRects() {
    MonitorEnumContext ctx;
    EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc, reinterpret_cast<LPARAM>(&ctx));
    return ctx.rects;
}

vector<WindowInfo> WindowScaler::GetActiveWindows() {
    vector<WindowInfo> windows;
    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        if (!WindowScaler::IsManagedAppWindow(hwnd)) return TRUE;

        auto* out = reinterpret_cast<vector<WindowInfo>*>(lParam);
        WindowInfo info;
        info.hwnd = hwnd;

        char title[512];
        GetWindowTextA(hwnd, title, sizeof(title));
        info.title = title;
        GetWindowRect(hwnd, &info.rect);
        GetWindowThreadProcessId(hwnd, &info.processId);
        QueryProcessImage(info.processId, info.processPath, info.processName);
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
    cout << "[MEMORY] Cached WINDOWPLACEMENT for HWND " << hwnd
         << " showCmd=" << state.placement.showCmd << endl;
}

bool WindowScaler::ApplyPlacementRect(HWND hwnd, const RECT& screenRect) {
    // Restore first so we never size an iconic window (GetWindowRect would be wrong).
    if (IsIconic(hwnd) || IsZoomed(hwnd)) {
        ShowWindow(hwnd, SW_RESTORE);
        Sleep(30);
    }

    const int width = screenRect.right - screenRect.left;
    const int height = screenRect.bottom - screenRect.top;

    // Absolute virtual-screen coordinates via SetWindowPos are the most reliable
    // snap path across monitors. Restore still uses cached WINDOWPLACEMENT.
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

void WindowScaler::SetPosition(HWND hwnd, int x, int y, int width, int height) {
    if (!hwnd || !IsWindow(hwnd)) return;
    CacheOriginalPosition(hwnd);
    RECT target{ x, y, x + width, y + height };
    ApplyPlacementRect(hwnd, target);
}

bool WindowScaler::SnapToBox(HWND hwnd, const SelectedBox& box) {
    if (!hwnd || !IsWindow(hwnd)) return false;

    const vector<RECT> monitorRects = GetWorkAreaRects();
    if (box.monitorIndex < 0 || box.monitorIndex >= static_cast<int>(monitorRects.size())) {
        cerr << "[SCALER] Invalid monitor index: " << box.monitorIndex << endl;
        return false;
    }

    CacheOriginalPosition(hwnd);

    const RECT& monRect = monitorRects[box.monitorIndex];
    const int monWidth = monRect.right - monRect.left;
    const int monHeight = monRect.bottom - monRect.top;

    RECT target{};
    target.left = monRect.left + static_cast<int>(box.relX * monWidth);
    target.top = monRect.top + static_cast<int>(box.relY * monHeight);
    target.right = target.left + static_cast<int>(box.relWidth * monWidth);
    target.bottom = target.top + static_cast<int>(box.relHeight * monHeight);

    cout << "[SCALER] Snap HWND " << hwnd << " -> LTRB "
         << target.left << "," << target.top << "," << target.right << "," << target.bottom << endl;

    return ApplyPlacementRect(hwnd, target);
}

void WindowScaler::PrepareCleanSlate(HWND dashboardHwnd, const unordered_set<HWND>& keepVisible) {
    cout << "[CLEAN] Preparing tracked clean slate..." << endl;
    const auto windows = GetActiveWindows();
    for (const auto& win : windows) {
        if (win.hwnd == dashboardHwnd) continue;
        if (keepVisible.count(win.hwnd)) continue;
        if (IsIconic(win.hwnd)) continue;

        CacheOriginalPosition(win.hwnd);
        ShowWindow(win.hwnd, SW_MINIMIZE);
        s_cleanSlateMinimized.insert(win.hwnd);
    }
}

void WindowScaler::ShowDesktop() {
    // Create-overlay path: minimize every managed window, still tracked for restore.
    PrepareCleanSlate(nullptr, {});
}

bool WindowScaler::RestoreWindowPosition(HWND hwnd) {
    auto it = s_originalPositions.find(hwnd);
    if (it == s_originalPositions.end()) return false;

    if (IsWindow(hwnd)) {
        WINDOWPLACEMENT placement = it->second.placement;
        placement.length = sizeof(WINDOWPLACEMENT);
        if (!SetWindowPlacement(hwnd, &placement)) {
            cerr << "[MEMORY] SetWindowPlacement restore failed for HWND " << hwnd << endl;
        } else {
            cout << "[MEMORY] Restored HWND " << hwnd << endl;
        }
    }

    s_originalPositions.erase(it);
    s_cleanSlateMinimized.erase(hwnd);
    return true;
}

void WindowScaler::RestoreAllCapturedWindows() {
    cout << "[MEMORY] Restoring snapped + clean-slate windows..." << endl;

    // Copy keys first — RestoreWindowPosition mutates the map.
    vector<HWND> cleanSlate(s_cleanSlateMinimized.begin(), s_cleanSlateMinimized.end());
    for (HWND hwnd : cleanSlate) {
        RestoreWindowPosition(hwnd);
    }

    while (!s_originalPositions.empty()) {
        RestoreWindowPosition(s_originalPositions.begin()->first);
    }

    s_cleanSlateMinimized.clear();
}

void WindowScaler::ClearSessionState() {
    s_originalPositions.clear();
    s_cleanSlateMinimized.clear();
}

string WindowScaler::ResolveAppPath(const string& processNameOrPath) {
    if (processNameOrPath.empty()) return "";

    string candidate = ExpandEnv(StripQuotes(processNameOrPath));
    if (candidate.find('\\') != string::npos || candidate.find('/') != string::npos) {
        return FileExists(candidate) ? candidate : candidate;
    }

    const string subKey = "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\" + candidate;
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
    if (SearchPathA(nullptr, candidate.c_str(), nullptr, MAX_PATH, pathBuffer, nullptr) > 0) {
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
                                    int timeoutMs) {
    const int stepMs = 200;
    const int attempts = max(1, timeoutMs / stepMs);

    for (int attempt = 0; attempt < attempts; ++attempt) {
        Sleep(stepMs);
        for (const auto& win : GetActiveWindows()) {
            if (excludeHwnds.count(win.hwnd)) continue;
            if (pid != 0 && win.processId == pid) return win.hwnd;
            if (!exeName.empty() && _stricmp(win.processName.c_str(), exeName.c_str()) == 0) {
                return win.hwnd;
            }
        }
    }
    return nullptr;
}

HWND WindowScaler::LaunchAndSnapApp(const string& assignedApp,
                                    const SelectedBox& box,
                                    const unordered_set<HWND>& excludeHwnds) {
    if (IsUnsupportedUwpBinding(assignedApp)) {
        cerr << "[LAUNCHER] Refusing ApplicationFrameHost.exe — save a real Win32 path or AUMID." << endl;
        return nullptr;
    }

    const string fullPath = ResolveAppPath(assignedApp);
    cout << "[LAUNCHER] Launching: " << fullPath << endl;

    if (!FileExists(fullPath)) {
        cerr << "[LAUNCHER] Path does not exist: " << fullPath << endl;
        return nullptr;
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
        SHELLEXECUTEINFOA sei = { sizeof(sei) };
        sei.fMask = SEE_MASK_NOCLOSEPROCESS;
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

    const string expectedName = filesystem::path(fullPath).filename().string();
    HWND hwnd = WaitForNewWindow(launchedPid, expectedName, knownBefore, 10000);
    if (!hwnd) {
        cerr << "[LAUNCHER] Timed out waiting for a new window from " << fullPath << endl;
        return nullptr;
    }

    cout << "[LAUNCHER] New HWND " << hwnd << " owned for snap" << endl;
    if (!SnapToBox(hwnd, box)) return nullptr;
    return hwnd;
}
