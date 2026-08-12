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
#include <unordered_set>

// Forward declaration to prevent circular header inclusions
struct SelectedBox;

// Taskbar-eligible top-level window snapshot used for matching and clean-slate.
struct WindowInfo {
    HWND hwnd = nullptr;
    DWORD processId = 0;
    std::string title;
    RECT rect{};
    std::string processName; // e.g. chrome.exe
    std::string processPath; // full image path when available
};

// Cached pre-snap / pre-minimize state. WINDOWPLACEMENT is the Win32 source of truth
// (FancyZones-style). Do NOT use GetWindowRect while a window is iconic.
struct OriginalWindowState {
    WINDOWPLACEMENT placement{};
};

class WindowScaler {
public:
    // --- Enumeration ---
    // Returns visible, root, non-toolwindow, non-cloaked windows with a title.
    static std::vector<WindowInfo> GetActiveWindows();

    // True if hwnd is a normal app window Biomes should manage.
    static bool IsManagedAppWindow(HWND hwnd);

    // --- Clean slate (replaces fake Win+D) ---
    // Minimizes every managed window except those in keepVisible / the dashboard.
    // Stores WINDOWPLACEMENT for each minimized window so Close can restore them.
    static void PrepareCleanSlate(HWND dashboardHwnd,
                                  const std::unordered_set<HWND>& keepVisible);

    // --- Snap / restore ---
    // Low-level move used by legacy BiomeManager helpers.
    static void SetPosition(HWND hwnd, int x, int y, int width, int height);

    // Caches placement once, then sizes hwnd into the box via SetWindowPos.
    static bool SnapToBox(HWND hwnd, const SelectedBox& box);

    // Restores one cached HWND, then forgets it.
    static bool RestoreWindowPosition(HWND hwnd);

    // Restores snapped windows AND clean-slate minimized windows.
    static void RestoreAllCapturedWindows();

    // Clears all session tracking without touching window positions.
    static void ClearSessionState();

    // --- Launch ---
    // Resolves registry App Paths / SearchPath; strips quotes and expands %ENV%.
    static std::string ResolveAppPath(const std::string& processNameOrPath);

    // True when the saved binding is ApplicationFrameHost / UWP frame without AUMID.
    static bool IsUnsupportedUwpBinding(const std::string& assignedApp);

    // Launch assignedApp and snap only a NEW hwnd for that process (never steals
    // an already-used Chrome/Electron window). excludeHwnds = already placed.
    // Returns the snapped HWND, or nullptr on failure.
    static HWND LaunchAndSnapApp(const std::string& assignedApp,
                                 const SelectedBox& box,
                                 const std::unordered_set<HWND>& excludeHwnds);

    // Legacy name kept for create-overlay flow: tracked minimize of all managed windows.
    static void ShowDesktop();

private:
    static std::unordered_map<HWND, OriginalWindowState> s_originalPositions;
    static std::unordered_set<HWND> s_cleanSlateMinimized;

    static void CacheOriginalPosition(HWND hwnd);
    static bool ApplyPlacementRect(HWND hwnd, const RECT& screenRect);
    static std::vector<RECT> GetWorkAreaRects();
    static bool GetProcessImage(DWORD pid, std::string& outPath, std::string& outName);
    static HWND WaitForNewWindow(DWORD pid,
                                 const std::string& exeName,
                                 const std::unordered_set<HWND>& excludeHwnds,
                                 int timeoutMs);
};

#endif // WINDOW_SCALER_HPP
