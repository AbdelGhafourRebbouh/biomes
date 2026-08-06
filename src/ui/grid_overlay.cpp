#define NOMINMAX
#include "../../include/ui/grid_overlay.hpp"
#include <iostream>
#include <algorithm>
#include <windows.h>

std::vector<MonitorInfoData> GridOverlay::s_monitors;
int GridOverlay::s_rows = 8;
int GridOverlay::s_cols = 14;
OverlayTheme GridOverlay::s_theme;

bool GridOverlay::s_isDragging = false;
POINT GridOverlay::s_dragStart = { 0, 0 };
POINT GridOverlay::s_dragCurrent = { 0, 0 };
HWND GridOverlay::s_activeDragHwnd = NULL;
std::vector<SelectedBox> GridOverlay::s_savedBoxes;

BOOL CALLBACK GridOverlay::MonitorEnumProc(HMONITOR hMonitor, HDC hdcMonitor, LPRECT lprcMonitor, LPARAM dwData) {
    MonitorInfoData info;
    info.index = static_cast<int>(s_monitors.size());
    info.hMonitor = hMonitor;
    info.rect = *lprcMonitor;
    info.hwndOverlay = NULL;
    s_monitors.push_back(info);
    return TRUE;
}

bool GridOverlay::ShowOverlay(int rows, int cols, const OverlayTheme& theme) {
    s_rows = rows;
    s_cols = cols;
    s_theme = theme;
    s_savedBoxes.clear();
    s_monitors.clear();

    // 1. Drop all active windows to reveal clean desktop wallpaper
    keybd_event(VK_LWIN, 0, 0, 0);
    keybd_event('D', 0, 0, 0);
    keybd_event('D', 0, KEYEVENTF_KEYUP, 0);
    keybd_event(VK_LWIN, 0, KEYEVENTF_KEYUP, 0);

    Sleep(150);

    // 2. Enumerate every active connected display
    EnumDisplayMonitors(NULL, NULL, MonitorEnumProc, 0);

    HINSTANCE hInstance = GetModuleHandle(NULL);
    const char CLASS_NAME[] = "BiomesPerMonitorGridOverlay";

    WNDCLASSA wc = {};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(NULL, IDC_CROSS);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);

    RegisterClassA(&wc);

    // 3. Create clean overlay window for EVERY detected monitor
    for (auto& mon : s_monitors) {
        int w = mon.rect.right - mon.rect.left;
        int h = mon.rect.bottom - mon.rect.top;

        mon.hwndOverlay = CreateWindowExA(
            WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW,
            CLASS_NAME,
            "Biomes Overlay",
            WS_POPUP | WS_VISIBLE,
            mon.rect.left, mon.rect.top, w, h,
            NULL, NULL, hInstance, NULL
        );

        if (mon.hwndOverlay) {
            SetLayeredWindowAttributes(mon.hwndOverlay, 0, s_theme.bgAlpha, LWA_ALPHA);
            SetWindowLongPtr(mon.hwndOverlay, GWLP_USERDATA, mon.index);
            ShowWindow(mon.hwndOverlay, SW_SHOW);
            UpdateWindow(mon.hwndOverlay);
        }
    }

    return !s_monitors.empty();
}

void GridOverlay::HideOverlay() {
    for (auto& mon : s_monitors) {
        if (mon.hwndOverlay) {
            DestroyWindow(mon.hwndOverlay);
            mon.hwndOverlay = NULL;
        }
    }
    s_monitors.clear();
}

void GridOverlay::RemoveOverlappingBoxes(int monitorIdx, const RECT& newRect) {
    auto it = s_savedBoxes.begin();
    while (it != s_savedBoxes.end()) {
        if (it->monitorIndex == monitorIdx) {
            RECT dummy;
            if (IntersectRect(&dummy, &it->pixelRect, &newRect)) {
                it = s_savedBoxes.erase(it);
                continue;
            }
        }
        ++it;
    }
}

void GridOverlay::DrawGrid(HDC hdc, HWND hwnd, int monitorIdx) {
    RECT clientRect;
    GetClientRect(hwnd, &clientRect);
    int width = clientRect.right;
    int height = clientRect.bottom;

    float cellWidth = static_cast<float>(width) / s_cols;
    float cellHeight = static_cast<float>(height) / s_rows;

    // 1. Draw Clean Full-Screen Grid Lines
    HPEN gridPen = CreatePen(PS_SOLID, s_theme.gridPenWidth, s_theme.gridLineColor);
    HPEN oldPen = (HPEN)SelectObject(hdc, gridPen);

    for (int i = 1; i < s_cols; ++i) {
        int x = static_cast<int>(i * cellWidth);
        MoveToEx(hdc, x, 0, NULL);
        LineTo(hdc, x, height);
    }
    for (int j = 1; j < s_rows; ++j) {
        int y = static_cast<int>(j * cellHeight);
        MoveToEx(hdc, 0, y, NULL);
        LineTo(hdc, width, y);
    }

    // 2. Draw Saved Boxes on this Monitor
    HPEN boxPen = CreatePen(PS_SOLID, s_theme.boxPenWidth, s_theme.boxBorderColor);
    HBRUSH boxBrush = CreateSolidBrush(s_theme.boxFillColor);
    SelectObject(hdc, boxPen);

    HFONT hFont = CreateFontA(16, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, ANSI_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                              DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
    HFONT oldFont = (HFONT)SelectObject(hdc, hFont);

    SetTextColor(hdc, s_theme.textColor);
    SetBkMode(hdc, TRANSPARENT);

    for (const auto& box : s_savedBoxes) {
        if (box.monitorIndex != monitorIdx) continue;

        RECT paddedRect = box.pixelRect;
        paddedRect.left += s_theme.boxPadding;
        paddedRect.top += s_theme.boxPadding;
        paddedRect.right -= s_theme.boxPadding;
        paddedRect.bottom -= s_theme.boxPadding;

        FillRect(hdc, &paddedRect, boxBrush);
        FrameRect(hdc, &paddedRect, boxBrush);

        std::string label = "Box #" + std::to_string(box.id);
        DrawTextA(hdc, label.c_str(), -1, &paddedRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    DeleteObject(boxPen);
    DeleteObject(boxBrush);

    // 3. Draw Active Dragging Region
    if (s_isDragging && s_activeDragHwnd == hwnd) {
        HPEN dragPen = CreatePen(PS_SOLID, s_theme.boxPenWidth, s_theme.boxBorderColor);
        HBRUSH dragBrush = CreateSolidBrush(s_theme.boxBorderColor);
        SelectObject(hdc, dragPen);

        RECT dragRect;
        dragRect.left = std::min(s_dragStart.x, s_dragCurrent.x);
        dragRect.top = std::min(s_dragStart.y, s_dragCurrent.y);
        dragRect.right = std::max(s_dragStart.x, s_dragCurrent.x);
        dragRect.bottom = std::max(s_dragStart.y, s_dragCurrent.y);

        int startCol = static_cast<int>(dragRect.left / cellWidth);
        int endCol = static_cast<int>(dragRect.right / cellWidth) + 1;
        int startRow = static_cast<int>(dragRect.top / cellHeight);
        int endRow = static_cast<int>(dragRect.bottom / cellHeight) + 1;

        RECT snappedRect;
        snappedRect.left = static_cast<int>(startCol * cellWidth);
        snappedRect.top = static_cast<int>(startRow * cellHeight);
        snappedRect.right = static_cast<int>(endCol * cellWidth);
        snappedRect.bottom = static_cast<int>(endRow * cellHeight);

        FrameRect(hdc, &snappedRect, dragBrush);

        DeleteObject(dragPen);
        DeleteObject(dragBrush);
    }

    SelectObject(hdc, oldFont);
    DeleteObject(hFont);
    SelectObject(hdc, oldPen);
    DeleteObject(gridPen);
}

LRESULT CALLBACK GridOverlay::WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    int monitorIdx = static_cast<int>(GetWindowLongPtr(hwnd, GWLP_USERDATA));

    switch (uMsg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);

            RECT rect;
            GetClientRect(hwnd, &rect);
            HDC memDC = CreateCompatibleDC(hdc);
            HBITMAP memBitmap = CreateCompatibleBitmap(hdc, rect.right, rect.bottom);
            HBITMAP oldBitmap = (HBITMAP)SelectObject(memDC, memBitmap);

            HBRUSH bgBrush = CreateSolidBrush(RGB(10, 18, 15));
            FillRect(memDC, &rect, bgBrush);
            DeleteObject(bgBrush);

            DrawGrid(memDC, hwnd, monitorIdx);

            BitBlt(hdc, 0, 0, rect.right, rect.bottom, memDC, 0, 0, SRCCOPY);

            SelectObject(memDC, oldBitmap);
            DeleteObject(memBitmap);
            DeleteDC(memDC);

            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_LBUTTONDOWN:
            s_isDragging = true;
            s_activeDragHwnd = hwnd;
            s_dragStart.x = LOWORD(lParam);
            s_dragStart.y = HIWORD(lParam);
            s_dragCurrent = s_dragStart;
            SetCapture(hwnd);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;

        case WM_MOUSEMOVE:
            if (s_isDragging && s_activeDragHwnd == hwnd) {
                s_dragCurrent.x = LOWORD(lParam);
                s_dragCurrent.y = HIWORD(lParam);
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;

        case WM_LBUTTONUP:
            if (s_isDragging && s_activeDragHwnd == hwnd) {
                s_isDragging = false;
                ReleaseCapture();

                RECT clientRect;
                GetClientRect(hwnd, &clientRect);
                float width = static_cast<float>(clientRect.right);
                float height = static_cast<float>(clientRect.bottom);
                float cellWidth = width / s_cols;
                float cellHeight = height / s_rows;

                RECT dragRect;
                dragRect.left = std::min(s_dragStart.x, s_dragCurrent.x);
                dragRect.top = std::min(s_dragStart.y, s_dragCurrent.y);
                dragRect.right = std::max(s_dragStart.x, s_dragCurrent.x);
                dragRect.bottom = std::max(s_dragStart.y, s_dragCurrent.y);

                int startCol = static_cast<int>(dragRect.left / cellWidth);
                int endCol = static_cast<int>(dragRect.right / cellWidth) + 1;
                int startRow = static_cast<int>(dragRect.top / cellHeight);
                int endRow = static_cast<int>(dragRect.bottom / cellHeight) + 1;

                RECT snappedRect;
                snappedRect.left = static_cast<int>(startCol * cellWidth);
                snappedRect.top = static_cast<int>(startRow * cellHeight);
                snappedRect.right = static_cast<int>(endCol * cellWidth);
                snappedRect.bottom = static_cast<int>(endRow * cellHeight);

                RemoveOverlappingBoxes(monitorIdx, snappedRect);

                SelectedBox newBox;
                newBox.id = static_cast<int>(s_savedBoxes.size()) + 1;
                newBox.monitorIndex = monitorIdx;
                newBox.startCol = startCol;
                newBox.endCol = endCol;
                newBox.startRow = startRow;
                newBox.endRow = endRow;
                newBox.pixelRect = snappedRect;

                // Calculate Normalized Relative Coordinates (0.0 to 1.0)
                newBox.relX = static_cast<float>(snappedRect.left) / width;
                newBox.relY = static_cast<float>(snappedRect.top) / height;
                newBox.relWidth = static_cast<float>(snappedRect.right - snappedRect.left) / width;
                newBox.relHeight = static_cast<float>(snappedRect.bottom - snappedRect.top) / height;

                s_savedBoxes.push_back(newBox);
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
       
        case WM_DISPLAYCHANGE: 
            std::cout << "[GRID OVERLAY] Display change detected! Refreshing overlay screen..." << std::endl;
            
            // Re-render this overlay window for the new resolution/display state
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;
        
    
        case WM_KEYDOWN:
            if (wParam == VK_BACK) {
                if (!s_savedBoxes.empty()) {
                    s_savedBoxes.pop_back();
                    for (auto& mon : s_monitors) InvalidateRect(mon.hwndOverlay, NULL, FALSE);
                }
            } else if (wParam == VK_ESCAPE) {
                HideOverlay();
                PostQuitMessage(0);
            }
            return 0;

        case WM_DESTROY:
            return 0;
            
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}
