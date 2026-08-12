#pragma once
#include <windows.h>
#include <vector>
#include <string>
#include <functional>

struct SelectedBox {
    int id = 0;
    int monitorIndex = 0;
    int startCol = 0, endCol = 0;
    int startRow = 0, endRow = 0;
    RECT pixelRect{};
    float relX = 0, relY = 0, relWidth = 0, relHeight = 0;

    // App binding — prefer full path; exeName/titleHint help matching on other PCs.
    std::string assignedApp;   // full path or exe name (legacy)
    std::string exeName;       // basename e.g. chrome.exe
    std::string titleHint;     // window title at assignment time
    std::string monitorDevice; // MONITORINFOEX.szDevice when available
    std::string aumid;         // reserved for UWP AppUserModelID
};

struct MonitorInfoData {
    int index = 0;
    HMONITOR hMonitor = nullptr;
    RECT rect{};
    HWND hwndOverlay = nullptr;
    std::string deviceName; // e.g. \\.\DISPLAY1
};

struct OverlayTheme {
    COLORREF gridLineColor  = RGB(120, 125, 140);
    COLORREF boxBorderColor = RGB(220, 230, 245);
    COLORREF boxFillColor   = RGB(55, 60, 72);
    COLORREF boxHoverColor  = RGB(120, 190, 175);
    COLORREF textColor      = RGB(245, 248, 255);
    BYTE bgAlpha            = 160;  // strong fullscreen veil over the whole monitor
    int boxPenWidth         = 2;
    int cornerRadius        = 16;   // ONLY user-drawn boxes
};

class GridOverlay {
public:
    static bool ShowOverlay(int rows = 8, int cols = 14, const OverlayTheme& theme = OverlayTheme());
    static void HideOverlay();
    static void SetCompletedCallback(std::function<void(const std::vector<SelectedBox>&)> cb);
    static void SetCancelledCallback(std::function<void()> cb);

private:
    static BOOL CALLBACK MonitorEnumProc(HMONITOR hMonitor, HDC hdcMonitor, LPRECT lprcMonitor, LPARAM dwData);
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    static void CALLBACK WinEventProc(HWINEVENTHOOK hWinEventHook, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD dwEventThread, DWORD dwmsEventTime);
    static void DrawGrid(HDC hdc, HWND hwnd, int mIdx);
    // Returns the zone under the screen-space cursor, or nullptr.
    static SelectedBox* HitTestBoxAtCursor(POINT screenPt);
    static void InvalidateAllOverlays();

    static std::vector<MonitorInfoData> s_monitors;
    static int s_rows, s_cols;
    static OverlayTheme s_theme;
    static bool s_isSnappingMode, s_isDragging;
    static POINT s_dragStart, s_dragCurrent;
    static HWND s_activeDragHwnd, s_movingWindowHwnd;
    static HWINEVENTHOOK s_hWinEventHook;
    static std::vector<SelectedBox> s_savedBoxes;
    static int s_hoveredBoxId; // -1 when none; updated while dragging in snap mode
    static std::function<void(const std::vector<SelectedBox>&)> s_onCompleted;
    static std::function<void()> s_onCancelled;
};
