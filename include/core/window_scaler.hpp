#pragma once
#ifndef WINDOW_SCALER_HPP
#define WINDOW_SCALER_HPP

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <string>
#include <vector>
#include <unordered_map>

// Forward declaration to prevent circular header inclusions
struct SelectedBox;

struct WindowInfo {
    HWND hwnd;
    std::string title;
    RECT rect;
    std::string processName;
};

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

    static void CacheOriginalPosition(HWND hwnd);
    static bool RestoreWindowPosition(HWND hwnd);
    static void RestoreAllCapturedWindows();

    static std::string ResolveAppPath(const std::string& processName);
    static bool LaunchAndSnapApp(const std::string& processName, const SelectedBox& box);

private:
    static std::unordered_map<HWND, OriginalWindowState> s_originalPositions;
};

#endif // WINDOW_SCALER_HPP