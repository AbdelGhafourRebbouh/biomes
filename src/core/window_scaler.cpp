#include "../../include/core/window_scaler.hpp"
#include <iostream>
#include <vector>
#include <string>
#include <unordered_map>
#include <windows.h>
#include <psapi.h>

using namespace std;

// Definition of static map matching header file definition
std::unordered_map<HWND, OriginalWindowState> WindowScaler::s_originalPositions;

struct MonitorEnumContext {
    vector<RECT> rects;
};

static BOOL CALLBACK ScalerMonitorEnumProc(HMONITOR hMonitor, HDC hdcMonitor, LPRECT lprcMonitor, LPARAM dwData) {
    auto* ctx = reinterpret_cast<MonitorEnumContext*>(dwData);
    MONITORINFO mi = { sizeof(MONITORINFO) };
    if (GetMonitorInfoA(hMonitor, &mi)) {
        ctx->rects.push_back(mi.rcMonitor);
    } else if (lprcMonitor) {
        ctx->rects.push_back(*lprcMonitor);
    }
    return TRUE;
}

static vector<RECT> GetSystemMonitorRects() {
    MonitorEnumContext ctx;
    EnumDisplayMonitors(NULL, NULL, ScalerMonitorEnumProc, reinterpret_cast<LPARAM>(&ctx));
    return ctx.rects;
}

BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
    if (!IsWindowVisible(hwnd)) return TRUE;

    char title[256];
    GetWindowTextA(hwnd, title, sizeof(title));
    if (strlen(title) > 0 && GetWindowTextLengthA(hwnd) > 0) {
        LONG exStyle = GetWindowLongA(hwnd, GWL_EXSTYLE);
        if (exStyle & WS_EX_TOOLWINDOW) return TRUE;

        auto* windows = reinterpret_cast<vector<WindowInfo>*>(lParam);

        RECT r;
        GetWindowRect(hwnd, &r); 

        DWORD processId;
        GetWindowThreadProcessId(hwnd, &processId);
        HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
        string exeName = "";
        if (hProcess) {
            char path[MAX_PATH];
            DWORD size = MAX_PATH;
            if (QueryFullProcessImageNameA(hProcess, 0, path, &size)) {
                string fullPath(path);
                size_t lastSlash = fullPath.find_last_of("\\/");
                if (lastSlash != string::npos) {
                    exeName = fullPath.substr(lastSlash + 1);
                }
            }
            CloseHandle(hProcess);
        }

        windows->push_back({ hwnd, string(title), r, exeName });
    }
    return TRUE;
}

void WindowScaler::SetPosition(HWND hwnd, int x, int y, int width, int height) {
    if (hwnd != NULL && IsWindow(hwnd)) {

        CacheOriginalPosition(hwnd);
        // Un-maximize / restore window if maximized or minimized
        if (IsZoomed(hwnd) || IsIconic(hwnd)) {
            ShowWindow(hwnd, SW_RESTORE);
        }

        // Remove WS_MAXIMIZE style bit explicitly
        LONG style = GetWindowLongA(hwnd, GWL_STYLE);
        if (style & WS_MAXIMIZE) {
            SetWindowLongA(hwnd, GWL_STYLE, style & ~WS_MAXIMIZE);
        }

        // Move and snap window
        BOOL res = SetWindowPos(
            hwnd, 
            HWND_TOP, 
            x, y, 
            width, height, 
            SWP_NOZORDER | SWP_SHOWWINDOW | SWP_FRAMECHANGED
        );

        if (!res) {
            cerr << "[SCALER ERROR] SetWindowPos failed for HWND " << hwnd 
                 << " (Win32 Error Code: " << GetLastError() << ")" << endl;
        }
    }
}

bool WindowScaler::SnapToBox(HWND hwnd, const SelectedBox& box) {
    if (!hwnd || !IsWindow(hwnd)) return false;
    vector<RECT> monitorRects = GetSystemMonitorRects();
    if (box.monitorIndex < 0 || box.monitorIndex >= static_cast<int>(monitorRects.size())) {
        cerr << "[SCALER] Invalid monitor index: " << box.monitorIndex << endl;
        return false;
    }

    const RECT& monRect = monitorRects[box.monitorIndex];
    int monWidth = monRect.right - monRect.left;
    int monHeight = monRect.bottom - monRect.top;

    int targetX = monRect.left + static_cast<int>(box.relX * monWidth);
    int targetY = monRect.top + static_cast<int>(box.relY * monHeight);
    int targetW = static_cast<int>(box.relWidth * monWidth);
    int targetH = static_cast<int>(box.relHeight * monHeight);
    cout << "[SCALER] Target Bounds -> X:" << targetX << " Y:" << targetY 
         << " W:" << targetW << " H:" << targetH << endl;
    SetPosition(hwnd, targetX, targetY, targetW, targetH);
    return true;
}

vector<WindowInfo> WindowScaler::GetActiveWindows() {
    vector<WindowInfo> windows;
    EnumWindows(EnumWindowsProc, reinterpret_cast<LPARAM>(&windows));
    return windows;
}

void WindowScaler::ShowDesktop() {
    keybd_event(VK_LWIN, 0, 0, 0);
    keybd_event('D', 0, 0, 0);
    keybd_event('D', 0, KEYEVENTF_KEYUP, 0);
    keybd_event(VK_LWIN, 0, KEYEVENTF_KEYUP, 0);
}

void WindowScaler::CacheOriginalPosition(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return;
    if (s_originalPositions.find(hwnd) == s_originalPositions.end()) {
        OriginalWindowState state;
        GetWindowRect(hwnd, &state.rect);
        state.isMaximized = (IsZoomed(hwnd) != FALSE);
        state.isMinimized = (IsIconic(hwnd) != FALSE);

        s_originalPositions[hwnd] = state;
        std::cout << "[MEMORY] Cached original bounds for HWND " << hwnd << std::endl;
    }
}

bool WindowScaler::RestoreWindowPosition(HWND hwnd) {
    auto it = s_originalPositions.find(hwnd);
    if (it == s_originalPositions.end() || !IsWindow(hwnd)) return false;
    const OriginalWindowState& state = it->second;

    if (state.isMaximized) {
        ShowWindow(hwnd, SW_MAXIMIZE);
    } else if (state.isMinimized) {
        ShowWindow(hwnd, SW_MINIMIZE);
    } else {
        ShowWindow(hwnd, SW_RESTORE);
        int w = state.rect.right - state.rect.left;
        int h = state.rect.bottom - state.rect.top;
        SetWindowPos(hwnd, HWND_TOP, state.rect.left, state.rect.top, w, h, SWP_NOZORDER | SWP_SHOWWINDOW);
    }

    s_originalPositions.erase(it);
    return true;
}

void WindowScaler::RestoreAllCapturedWindows() {
    std::cout << "[MEMORY] Restoring captured windows to original bounds..." << std::endl;
    while (!s_originalPositions.empty()) {
        HWND hwnd = s_originalPositions.begin()->first;
        RestoreWindowPosition(hwnd);
    }
}

std::string WindowScaler::ResolveAppPath(const std::string& processName) {
    if (processName.empty()) return "";
    if (processName.find("\\") != std::string::npos || processName.find("/") != std::string::npos) {
        return processName;
    }

    HKEY hKey;
    std::string subKey = "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\" + processName;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, subKey.c_str(), 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        char pathBuffer[MAX_PATH];
        DWORD bufferSize = sizeof(pathBuffer);
        if (RegQueryValueExA(hKey, NULL, NULL, NULL, reinterpret_cast<LPBYTE>(pathBuffer), &bufferSize) == ERROR_SUCCESS) {
            RegCloseKey(hKey);
            return std::string(pathBuffer);
        }
        RegCloseKey(hKey);
    }

    return processName;
}