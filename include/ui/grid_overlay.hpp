#pragma once
#include <windows.h>
#include <vector>
#include <string>

// Relative / Normalized Box Structure (Resolution & Monitor Agnostic)
struct SelectedBox {
    int id;
    int monitorIndex;
    
    // Grid cell positions
    int startCol, endCol;
    int startRow, endRow;
    
    // Pixel bounds on current display
    RECT pixelRect;

    // Relative Normalized Coordinates (0.0f to 1.0f) for dynamic resizing
    float relX;
    float relY;
    float relWidth;
    float relHeight;

    // Executable binding (e.g. "Code.exe", "Obsidian.exe")
    std::string assignedApp = "";
};

struct MonitorInfoData {
    int index;
    HMONITOR hMonitor;
    RECT rect;
    HWND hwndOverlay;
};

// Centralized Theme - Easy to restyle with your visual identity later
struct OverlayTheme {
    COLORREF gridLineColor  = RGB(0, 180, 160);   // Subtle grid guide color
    COLORREF boxBorderColor = RGB(0, 255, 170);   // Neon active box border
    COLORREF boxFillColor   = RGB(12, 35, 28);    // Card fill color
    COLORREF textColor      = RGB(240, 255, 245); // Text color
    BYTE bgAlpha            = 180;                // Transparency (0 = invisible, 255 = solid)
    int gridPenWidth        = 1;
    int boxPenWidth         = 2;
    int boxPadding          = 2;                  // Inner gap margin between boxes
};

class GridOverlay {
public:
    static bool ShowOverlay(int rows = 8, int cols = 14, const OverlayTheme& theme = OverlayTheme());
    static void HideOverlay();
    static std::vector<SelectedBox> GetSavedBoxes() { return s_savedBoxes; }

private:
    static BOOL CALLBACK MonitorEnumProc(HMONITOR hMonitor, HDC hdcMonitor, LPRECT lprcMonitor, LPARAM dwData);
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    static void DrawGrid(HDC hdc, HWND hwnd, int monitorIdx);
    static void RemoveOverlappingBoxes(int monitorIdx, const RECT& newRect);

    static std::vector<MonitorInfoData> s_monitors;
    static int s_rows;
    static int s_cols;
    static OverlayTheme s_theme;

    static bool s_isDragging;
    static POINT s_dragStart;
    static POINT s_dragCurrent;
    static HWND s_activeDragHwnd;
    static std::vector<SelectedBox> s_savedBoxes;
};