#include "../../include/core/window_scaler.hpp"
#include <iostream>


// Callback function executed by Windows for every top-level window found
BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
    // Skip invisible background windows (returning TRUE tells Windows to continue the search loop)
    if (!IsWindowVisible(hwnd)) return TRUE;

    char title[256];
    GetWindowTextA(hwnd, title, sizeof(title));

    if (strlen(title) > 0 && GetWindowTextLengthA(hwnd) > 0) {
        auto* windows = reinterpret_cast<std::vector<WindowInfo>*>(lParam);

        /*reinterpret_cast forces the compiler to re-interpret the bit 
        pattern of lParam directly as the target type without 
        modifying any underlying data*/

        // lParam is a generic 64-bit integer / void pointer

        RECT r;
        //window's screen coordinates

        GetWindowRect(hwnd, &r); 
        windows->push_back({ hwnd, std::string(title), r });
    }
    return TRUE;
}


// Safely moves and resizes a valid window without changing its layering order (Z-order)
void WindowScaler::SetPosition(HWND hwnd, int x, int y, int width, int height) {
    // Verify window handle exists before trying to modify it
    if (hwnd != NULL && IsWindow(hwnd)) {
        // Move/resize window: SWP_NOZORDER keeps its current layering position
        SetWindowPos(hwnd, HWND_TOP, x, y, width, height, SWP_NOZORDER | SWP_SHOWWINDOW);
    }
}


// Scans the OS and returns a list of all visible top-level windows
std::vector<WindowInfo> WindowScaler::GetActiveWindows() {
    std::vector<WindowInfo> windows;
    EnumWindows(EnumWindowsProc, reinterpret_cast<LPARAM>(&windows));
    return windows;
}
// pressing the shourcut keys win + D to minimize all windows and show the desktop
void WindowScaler::ShowDesktop() {
    // 1. Press down Left Windows key
    keybd_event(VK_LWIN, 0, 0, 0);

    // 2. Press down 'D' key
    keybd_event('D', 0, 0, 0);

    // 3. Release 'D' key
    keybd_event('D', 0, KEYEVENTF_KEYUP, 0);

    // 4. Release Left Windows key
    keybd_event(VK_LWIN, 0, KEYEVENTF_KEYUP, 0);
}