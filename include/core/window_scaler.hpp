#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include "../ui/grid_overlay.hpp"

/// Represents metadata for a single captured desktop window
struct WindowInfo {
    HWND hwnd;               // Unique handle (ID) identifying the window in Windows OS
    std::string title;       // Title bar text of the window
    RECT rect;               // Screen position and dimensions
    std::string processName; // Executable process name (e.g., "Obsidian.exe", "Code.exe")
};

class WindowScaler {
public:
    // Moves and resizes an open window handle
    static void SetPosition(HWND hwnd, int x, int y, int width, int height);

    // Snaps a window to a SelectedBox target region on its designated monitor
    static bool SnapToBox(HWND hwnd, const SelectedBox& box);

    // Scans all open top-level application windows on Windows OS
    static std::vector<WindowInfo> GetActiveWindows();

    // Minimizes all active windows to expose the desktop background
    static void ShowDesktop();
};