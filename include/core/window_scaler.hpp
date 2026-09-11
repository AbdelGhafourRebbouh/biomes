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
#include <functional>

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
    DWORD processId = 0;
    WINDOWPLACEMENT preBiomePlacement{};
    bool hadPreBiomeState = false; // false when launched fresh during this session
};

class WindowScaler {
public:
    static std::vector<WindowInfo> GetActiveWindows();

    // Pure screen-pixel geometry. The application runs per-monitor DPI aware;
    // rcWork already contains physical coordinates and must not be scaled twice.
    static bool CalculateRelativeRect(const RECT& work, double x, double y,
                                     double width, double height, RECT& target);
    // Half-open grid edges: halves = 2 columns, thirds = 3, quadrants = 2 x 2.
    static bool CalculateGridRect(const RECT& work, int rows, int columns,
                                  int startRow, int endRow, int startColumn,
                                  int endColumn, RECT& target);

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

    // Requests placement; the timer verifies restore, geometry and renderer settling.
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
    // Delay background dispatch/placement until activation finishes its clean-slate pass.
    static void PauseLaunchTracking(bool paused);
    static void SetLaunchProgressCallback(std::function<void(const std::string&)> callback);
    static void BeginLaunchProgress(const std::string& name, const std::vector<SelectedBox>& boxes);
    static void FinishLaunchSetup();
    static void ReportLaunchState(const SelectedBox& box, const std::string& state, const std::string& detail = "");

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
