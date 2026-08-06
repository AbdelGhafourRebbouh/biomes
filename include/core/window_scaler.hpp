#pragma once
#ifndef WINDOW_SCALER_HPP
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WINDOW_SCALER_HPP

#include <windows.h>
#include <string>
#include <vector>
#include <unordered_map>
#include "../ui/grid_overlay.hpp"

struct WindowInfo {
    HWND hwnd;
    std::string title;
    RECT rect;
    std::string processName;
};

// Window state storage for restoration
struct OriginalWindowState {
    RECT rect;
    bool isMaximized;
    bool isMinimized;
};

class WindowScaler {
public:
    static void SetPosition(HWND hwnd, int x, int y, int width, int height);
    static bool SnapToBox(HWND hwnd, const SelectedBox& box);
    static std::vector<WindowInfo> GetActiveWindows();
    static void ShowDesktop();

    // NEW: Restoration Memory
    static void CacheOriginalPosition(HWND hwnd);
    static bool RestoreWindowPosition(HWND hwnd);
    static void RestoreAllCapturedWindows();

    // NEW: Registry Path Resolver
    static std::string ResolveAppPath(const std::string& processName);
    static bool LaunchAndSnapApp(const std::string& processName, const SelectedBox& box);

private:
    static std::unordered_map<HWND, OriginalWindowState> s_originalPositions;
};

#endif // WINDOW_SCALER_HPP