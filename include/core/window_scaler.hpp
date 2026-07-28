#pragma once
#include <windows.h>
#include <string>
#include <vector>
using namespace std;

struct WindowInfo {
    HWND hwnd;
    string title;
    RECT rect;
};

class WindowScaler {
public:
    // Moves and resizes an open window handle
    static void SetPosition(HWND hwnd, int x, int y, int width, int height);

    // Scans all open top-level application windows on Windows OS
    static vector<WindowInfo> GetActiveWindows();

    // Minimizes all active windows to expose the desktop background
    static void ShowDesktop();
};