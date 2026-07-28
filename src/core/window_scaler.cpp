#include "../../include/core/window_scaler.hpp"
#include <iostream>
using namespace std;

BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
    if (!IsWindowVisible(hwnd)) return TRUE;

    char title[256];
    GetWindowTextA(hwnd, title, sizeof(title));

    if (strlen(title) > 0 && GetWindowTextLengthA(hwnd) > 0) {
        auto* windows = reinterpret_cast<vector<WindowInfo>*>(lParam);
        RECT r;
        GetWindowRect(hwnd, &r);
        windows->push_back({ hwnd, string(title), r });
    }
    return TRUE;
}

void WindowScaler::SetPosition(HWND hwnd, int x, int y, int width, int height) {
    if (hwnd != NULL && IsWindow(hwnd)) {
        SetWindowPos(hwnd, HWND_TOP, x, y, width, height, SWP_NOZORDER | SWP_SHOWWINDOW);
    }
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