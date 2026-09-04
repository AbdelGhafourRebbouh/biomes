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

struct SelectedBox;

struct WindowInfo {
    HWND hwnd = nullptr;
    DWORD processId = 0;
    std::string title;
    RECT rect{};
    std::string processName;
    std::string processPath;
    std::string aumid; // Store/UWP Application User Model ID when available
};

// Separates the visible HWND that must be moved from the process identity used
// to save, launch, and match packaged applications.
struct WindowIdentity {
    HWND placementHwnd = nullptr;
    DWORD processId = 0;
    std::string processName;
    std::string processPath;
    std::string aumid;
    bool isApplicationFrameHost = false;
};

struct OriginalWindowState {
    WINDOWPLACEMENT placement{};
};

struct BiomeAppSession {
    HWND hwnd = nullptr;
    WINDOWPLACEMENT preBiomePlacement{};
    bool hadPreBiomeState = false; // false when launched fresh during this session
};

class WindowScaler {
public:
    static std::vector<WindowInfo> GetActiveWindows();

    // Resolves a UWP host wrapper to its actual package identity while retaining
    // the outer HWND for placement. Returns false for an unresolved host wrapper.
    static bool ResolveWindowIdentity(HWND hwnd, WindowIdentity& outIdentity);

    static bool IsManagedAppWindow(HWND hwnd);

    // Prefer main application windows; filters small Electron helper HWNDs.
    static bool IsMainApplicationWindow(HWND hwnd);

    static bool IsExplorerProcess(const std::string& exeOrPath);

    static void MinimizeExceptPlaced(const std::vector<HWND>& placedHwnds,
                                   HWND dashboardHwnd = nullptr);

    static void MinimizeExeSiblings(const std::string& exeName, HWND keepHwnd);

    static bool IsOurProcessWindow(HWND hwnd);

    // Create-flow: minimize managed apps only; never Shell MinimizeAll (breaks dashboard).
    static void PrepareForOverlayCreate(HWND dashboardHwnd);

    static void PrepareCleanSlate(HWND dashboardHwnd,
                                  const std::unordered_set<HWND>& keepVisible);

    // Applies saved zone geometry (SetWindowPos + fullscreen exit for Electron).
    static bool ForceSnapToBox(HWND hwnd, const SelectedBox& box);

    // Record pre-biome state before first snap this session.
    static void CacheBiomeAppPreState(HWND hwnd, bool launchedFresh);

    // Restore pre-biome placement then minimize each biome app; leave clean-slate windows alone.
    static void CloseBiomeSession();

    static void RaiseBiomeWindows(const std::vector<HWND>& biomeHwnds);

    static std::string ResolveAppPath(const std::string& processNameOrPath);

    static bool IsUnsupportedUwpBinding(const std::string& assignedApp);

    static HWND LaunchAndSnapApp(const std::string& assignedApp,
                                 const SelectedBox& box,
                                 const std::unordered_set<HWND>& excludeHwnds,
                                 int waitTimeoutMs = 15000);

    // Starts a missing application immediately and snaps its eventual workspace
    // window through WinEvent notifications without blocking Biome activation.
    static bool LaunchAndTrackApp(const std::string& assignedApp,
                                  const SelectedBox& box,
                                  const std::unordered_set<HWND>& excludeHwnds,
                                  std::string& outError);

    // Discards deferred launch work when a Biome is closed or replaced.
    static void CancelPendingLaunches();

private:
    static std::unordered_map<HWND, OriginalWindowState> s_originalPositions;
    static std::unordered_set<HWND> s_cleanSlateMinimized;
    static std::unordered_map<HWND, BiomeAppSession> s_biomeAppSessions;

    static void CacheOriginalPosition(HWND hwnd);
    static bool ApplyPlacementRect(HWND hwnd, const RECT& screenRect);
    static bool ComputeTargetRect(const SelectedBox& box, RECT& outTarget);
    static bool GetProcessImage(DWORD pid, std::string& outPath, std::string& outName);
    static HWND WaitForNewWindow(DWORD pid,
                                 const std::string& exeName,
                                 const std::unordered_set<HWND>& excludeHwnds,
                                 const std::vector<std::string>& expectedAumids,
                                 int timeoutMs);
};

#endif // WINDOW_SCALER_HPP
