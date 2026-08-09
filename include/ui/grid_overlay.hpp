#pragma once
#include <windows.h>
#include <vector>
#include <string>
#include <functional>

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
    COLORREF gridLineColor  = RGB(100, 200, 180);  // Subtle grid guide color
    COLORREF boxBorderColor = RGB(80, 220, 180);   // Active box border
    COLORREF boxFillColor   = RGB(20, 50, 45);     // Semi-transparent box fill
    COLORREF boxHoverColor  = RGB(40, 80, 70);     // Hover state highlight
    COLORREF textColor      = RGB(200, 255, 240);  // Text color
    BYTE bgAlpha            = 60;                  // Very transparent (allows wallpaper through)
    BYTE boxAlpha           = 80;                  // Box transparency
    int gridPenWidth        = 1;
    int boxPenWidth         = 2;
    int boxPadding          = 2;
};

enum class SplitDirection {
    Horizontal,
    Vertical
};

class GridOverlay {
public:
    static bool ShowOverlay(int rows = 8, int cols = 14, const OverlayTheme& theme = OverlayTheme());
    static void HideOverlay();
    static std::vector<SelectedBox> GetSavedBoxes() { return s_savedBoxes; }
    static void SetCompletedCallback(std::function<void(const std::vector<SelectedBox>&)> callback);
    static void SetCancelledCallback(std::function<void()> callback);
    static std::vector<RECT> SplitRect(const RECT& rect, SplitDirection direction, int splitPercent = 50);

private:
    static void PopulateBoxFromRect(SelectedBox& box, int monitorIndex, const RECT& rect, int id, const RECT& monitorRect);
    static void PushCurrentBoxesToHistory();
    static void RestoreLastSplitState();

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
    static int s_hoveredBoxIndex;           // Track which box is being hovered (for visual feedback)
    static HWND s_draggedWindowHwnd;        // Track which taskbar window is being dragged
    static std::string s_draggedWindowName; // Store app name being dragged
    static std::vector<SelectedBox> s_savedBoxes;
    static std::vector<std::vector<SelectedBox>> s_splitHistory;
    static std::function<void(const std::vector<SelectedBox>&)> s_onCompleted;
    static std::function<void()> s_onCancelled;
};
