#include "../../include/core/window_scaler.hpp"
#include <iostream>
#include <vector>
#include <string>
#include <windows.h>
#include <psapi.h>

using namespace std;

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
        // 1. Un-maximize / restore window if maximized or minimized
        // (Windows OS silently ignores SetWindowPos on MAXIMIZED windows!)
        if (IsZoomed(hwnd) || IsIconic(hwnd)) {
            ShowWindow(hwnd, SW_RESTORE);
        }

        // 2. Remove WS_MAXIMIZE style bit explicitly
        LONG style = GetWindowLongA(hwnd, GWL_STYLE);
        if (style & WS_MAXIMIZE) {
            SetWindowLongA(hwnd, GWL_STYLE, style & ~WS_MAXIMIZE);
        }

        // 3. Move and snap window
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