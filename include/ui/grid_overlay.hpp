#pragma once
#include <windows.h>
#include <vector>
#include <string>
#include <functional>

struct SelectedBox {
    int id;
    int monitorIndex;
    int startCol, endCol;
    int startRow, endRow;
    RECT pixelRect;
    float relX, relY, relWidth, relHeight;
    std::string assignedApp = "";
};

struct MonitorInfoData {
    int index;
    HMONITOR hMonitor;
    RECT rect;
    HWND hwndOverlay;
};

struct OverlayTheme {
    COLORREF gridLineColor  = RGB(100, 100, 100);
    COLORREF boxBorderColor = RGB(0, 255, 150);
    COLORREF boxFillColor   = RGB(30, 30, 30);
    COLORREF boxHoverColor  = RGB(0, 255, 150);
    COLORREF textColor      = RGB(255, 255, 255);
    BYTE bgAlpha            = 120;
    int boxPenWidth         = 2;
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

    static std::vector<MonitorInfoData> s_monitors;
    static int s_rows, s_cols;
    static OverlayTheme s_theme;
    static bool s_isSnappingMode, s_isDragging;
    static POINT s_dragStart, s_dragCurrent;
    static HWND s_activeDragHwnd, s_movingWindowHwnd;
    static HWINEVENTHOOK s_hWinEventHook;
    static std::vector<SelectedBox> s_savedBoxes;
    static std::function<void(const std::vector<SelectedBox>&)> s_onCompleted;
    static std::function<void()> s_onCancelled;
};
