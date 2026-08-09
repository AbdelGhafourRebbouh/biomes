#define NOMINMAX
#include "../../include/ui/grid_overlay.hpp"
#include <iostream>
#include <algorithm>
#include <fstream>
#include <windows.h>

std::vector<MonitorInfoData> GridOverlay::s_monitors;
int GridOverlay::s_rows = 8;
int GridOverlay::s_cols = 14;
OverlayTheme GridOverlay::s_theme;
bool GridOverlay::s_isSnappingMode = false;
bool GridOverlay::s_isDragging = false;
POINT GridOverlay::s_dragStart = { 0, 0 };
POINT GridOverlay::s_dragCurrent = { 0, 0 };
HWND GridOverlay::s_activeDragHwnd = NULL;
HWND GridOverlay::s_movingWindowHwnd = NULL;
HWINEVENTHOOK GridOverlay::s_hWinEventHook = NULL;
std::vector<SelectedBox> GridOverlay::s_savedBoxes;
std::vector<std::vector<SelectedBox>> GridOverlay::s_splitHistory;
std::function<void(const std::vector<SelectedBox>&)> GridOverlay::s_onCompleted;
std::function<void()> GridOverlay::s_onCancelled;

void CALLBACK GridOverlay::WinEventProc(HWINEVENTHOOK hWinEventHook, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD dwEventThread, DWORD dwmsEventTime) {
    if (!s_isSnappingMode || idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;
    
    if (event == EVENT_SYSTEM_MOVESIZESTART) {
        s_movingWindowHwnd = hwnd;
        // When dragging starts, bring the "Ghost Grid" to the front but keep it non-interactive
        for (auto& mon : s_monitors) {
            SetWindowPos(mon.hwndOverlay, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            InvalidateRect(mon.hwndOverlay, NULL, FALSE);
        }
    }
    else if (event == EVENT_OBJECT_LOCATIONCHANGE) {
        if (s_movingWindowHwnd == hwnd) {
            for (auto& mon : s_monitors) InvalidateRect(mon.hwndOverlay, NULL, FALSE);
        }
    }
    else if (event == EVENT_SYSTEM_MOVESIZEEND) {
        if (s_movingWindowHwnd == hwnd) {
            POINT pt; GetCursorPos(&pt);
            for (auto& box : s_savedBoxes) {
                const auto& mon = s_monitors[box.monitorIndex];
                RECT sb = box.pixelRect; OffsetRect(&sb, mon.rect.left, mon.rect.top);
                if (PtInRect(&sb, pt)) {
                    SetWindowPos(hwnd, HWND_TOP, sb.left, sb.top, sb.right - sb.left, sb.bottom - sb.top, SWP_SHOWWINDOW);
                    DWORD pid; GetWindowThreadProcessId(hwnd, &pid);
                    HANDLE hP = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
                    if (hP) { char p[MAX_PATH]; DWORD sz = MAX_PATH; if (QueryFullProcessImageNameA(hP, 0, p, &sz)) { box.assignedApp = std::string(p); } CloseHandle(hP); }
                    break;
                }
            }
            s_movingWindowHwnd = NULL;
            // Push grid back after drop so it doesn't block other apps
            for (auto& mon : s_monitors) {
                SetWindowPos(mon.hwndOverlay, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                InvalidateRect(mon.hwndOverlay, NULL, FALSE);
            }
        }
    }
}

BOOL CALLBACK GridOverlay::MonitorEnumProc(HMONITOR hMonitor, HDC hdcMonitor, LPRECT lprcMonitor, LPARAM dwData) {
    MonitorInfoData info; info.index = (int)s_monitors.size(); info.hMonitor = hMonitor; info.rect = *lprcMonitor; info.hwndOverlay = NULL;
    s_monitors.push_back(info); return TRUE;
}

bool GridOverlay::ShowOverlay(int rows, int cols, const OverlayTheme& theme) {
    s_rows = rows; s_cols = cols; s_theme = theme; s_savedBoxes.clear(); s_monitors.clear(); s_isSnappingMode = false;
    EnumDisplayMonitors(NULL, NULL, MonitorEnumProc, 0);
    HINSTANCE hInst = GetModuleHandle(NULL);
    WNDCLASSA wc = {}; wc.lpfnWndProc = WindowProc; wc.hInstance = hInst; wc.lpszClassName = "BiomesGridOverlay"; wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);
    s_hWinEventHook = SetWinEventHook(EVENT_SYSTEM_MOVESIZESTART, EVENT_SYSTEM_MOVESIZEEND, NULL, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, NULL, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    for (auto& mon : s_monitors) {
        mon.hwndOverlay = CreateWindowExA(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, "BiomesGridOverlay", "Overlay", WS_POPUP | WS_VISIBLE, mon.rect.left, mon.rect.top, mon.rect.right-mon.rect.left, mon.rect.bottom-mon.rect.top, NULL, NULL, hInst, NULL);
        SetLayeredWindowAttributes(mon.hwndOverlay, 0, s_theme.bgAlpha, LWA_ALPHA);
        SetWindowLongPtr(mon.hwndOverlay, GWLP_USERDATA, mon.index);
    }
    return true;
}

void GridOverlay::HideOverlay() {
    if (s_hWinEventHook) { UnhookWinEvent(s_hWinEventHook); s_hWinEventHook = NULL; }
    for (auto& mon : s_monitors) if (mon.hwndOverlay) DestroyWindow(mon.hwndOverlay);
    s_monitors.clear();
}

void GridOverlay::SetCompletedCallback(std::function<void(const std::vector<SelectedBox>&)> cb) { s_onCompleted = std::move(cb); }
void GridOverlay::SetCancelledCallback(std::function<void()> cb) { s_onCancelled = std::move(cb); }

void GridOverlay::RemoveOverlappingBoxes(int mIdx, const RECT& nr) {
    auto it = s_savedBoxes.begin();
    while (it != s_savedBoxes.end()) {
        if (it->monitorIndex == mIdx) { RECT d; if (IntersectRect(&d, &it->pixelRect, &nr)) { it = s_savedBoxes.erase(it); continue; } }
        ++it;
    }
}

void GridOverlay::DrawGrid(HDC hdc, HWND hwnd, int mIdx) {
    RECT r; GetClientRect(hwnd, &r);
    
    // In Snapping Mode, we don't draw the background or the grid lines, just the boxes.
    if (!s_isSnappingMode) {
        float cw = (float)r.right / s_cols; float ch = (float)r.bottom / s_rows;
        HPEN gPen = CreatePen(PS_SOLID, 1, s_theme.gridLineColor); SelectObject(hdc, gPen);
        for (int i=1; i<s_cols; ++i) { MoveToEx(hdc, (int)(i*cw), 0, NULL); LineTo(hdc, (int)(i*cw), r.bottom); }
        for (int j=1; j<s_rows; ++j) { MoveToEx(hdc, 0, (int)(j*ch), NULL); LineTo(hdc, r.right, (int)(j*ch)); }
        DeleteObject(gPen);
    }

    HBRUSH bBrush = CreateSolidBrush(s_theme.boxFillColor); 
    HBRUSH hBrush = CreateSolidBrush(s_theme.boxHoverColor);
    HPEN bPen = CreatePen(PS_SOLID, s_theme.boxPenWidth, s_theme.boxBorderColor); 
    SelectObject(hdc, bPen);
    
    POINT m; GetCursorPos(&m);
    for (auto& box : s_savedBoxes) {
        if (box.monitorIndex != mIdx) continue;
        RECT b = box.pixelRect; 
        RECT sb = b; OffsetRect(&sb, s_monitors[mIdx].rect.left, s_monitors[mIdx].rect.top);
        
        bool ih = (s_isSnappingMode && s_movingWindowHwnd && PtInRect(&sb, m));
        
        // In snap mode, empty boxes are transparent until hovered
        if (s_isSnappingMode) {
            if (ih) {
                SelectObject(hdc, hBrush);
                Rectangle(hdc, b.left, b.top, b.right, b.bottom);
            } else {
                // Just draw a faint border for the box
                FrameRect(hdc, &b, (HBRUSH)GetStockObject(WHITE_BRUSH));
            }
        } else {
            SelectObject(hdc, bBrush);
            Rectangle(hdc, b.left, b.top, b.right, b.bottom);
        }

        if (!box.assignedApp.empty()) { 
            std::string n = box.assignedApp.substr(box.assignedApp.find_last_of("\\/") + 1); 
            SetTextColor(hdc, RGB(255,255,255));
            DrawTextA(hdc, n.c_str(), -1, &b, DT_CENTER | DT_VCENTER | DT_SINGLELINE); 
        }
    }

    if (!s_isSnappingMode && s_isDragging && s_activeDragHwnd == hwnd) {
        HPEN dPen = CreatePen(PS_DOT, 1, RGB(255,255,255)); SelectObject(hdc, dPen);
        Rectangle(hdc, std::min((long)s_dragStart.x, (long)s_dragCurrent.x), std::min((long)s_dragStart.y, (long)s_dragCurrent.y), std::max((long)s_dragStart.x, (long)s_dragCurrent.x), std::max((long)s_dragStart.y, (long)s_dragCurrent.y));
        DeleteObject(dPen);
    }
    
    DeleteObject(bBrush); DeleteObject(hBrush); DeleteObject(bPen);
}

LRESULT CALLBACK GridOverlay::WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    int mIdx = (int)GetWindowLongPtr(hwnd, GWLP_USERDATA);
    switch (uMsg) {
        case WM_PAINT: {
            PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps); RECT r; GetClientRect(hwnd, &r);
            HDC mdc = CreateCompatibleDC(hdc); HBITMAP mb = CreateCompatibleBitmap(hdc, r.right, r.bottom); SelectObject(mdc, mb);
            
            // Phase 1 has a background. Phase 2 (Snapping) is completely transparent except for boxes.
            if (!s_isSnappingMode) {
                HBRUSH bg = CreateSolidBrush(RGB(20,20,20)); FillRect(mdc, &r, bg); DeleteObject(bg);
            } else {
                // Transparent background for snap mode
                HBRUSH bg = CreateSolidBrush(RGB(0,0,0)); FillRect(mdc, &r, bg); DeleteObject(bg);
            }

            DrawGrid(mdc, hwnd, mIdx); 
            BitBlt(hdc, 0, 0, r.right, r.bottom, mdc, 0, 0, SRCCOPY);
            DeleteObject(mb); DeleteDC(mdc); EndPaint(hwnd, &ps); return 0;
        }
        case WM_NCHITTEST: {
            if (s_isSnappingMode) return HTTRANSPARENT;
            return HTCLIENT;
        }
        case WM_LBUTTONDOWN:
            if (!s_isSnappingMode) {
                s_isDragging = true; s_activeDragHwnd = hwnd;
                s_dragStart = { LOWORD(lParam), HIWORD(lParam) }; s_dragCurrent = s_dragStart;
                SetCapture(hwnd);
            }
            return 0;
        case WM_MOUSEMOVE:
            if (s_isDragging) { s_dragCurrent = { LOWORD(lParam), HIWORD(lParam) }; InvalidateRect(hwnd, NULL, FALSE); }
            return 0;
        case WM_LBUTTONUP:
            if (s_isDragging) {
                s_isDragging = false; ReleaseCapture();
                RECT r; GetClientRect(hwnd, &r);
                float cw = (float)r.right/s_cols; float ch = (float)r.bottom/s_rows;
                int sc = (int)(std::min((long)s_dragStart.x, (long)s_dragCurrent.x)/cw), ec = (int)(std::max((long)s_dragStart.x, (long)s_dragCurrent.x)/cw)+1;
                int sr = (int)(std::min((long)s_dragStart.y, (long)s_dragCurrent.y)/ch), er = (int)(std::max((long)s_dragStart.y, (long)s_dragCurrent.y)/ch)+1;
                SelectedBox b; b.id = (int)s_savedBoxes.size()+1; b.monitorIndex = mIdx; b.startCol = sc; b.endCol = ec; b.startRow = sr; b.endRow = er;
                b.pixelRect = { (int)(sc*cw), (int)(sr*ch), (int)(ec*cw), (int)(er*ch) };
                b.relX = (float)b.pixelRect.left/r.right; b.relY = (float)b.pixelRect.top/r.bottom;
                b.relWidth = (float)(b.pixelRect.right-b.pixelRect.left)/r.right; b.relHeight = (float)(b.pixelRect.bottom-b.pixelRect.top)/r.bottom;
                RemoveOverlappingBoxes(mIdx, b.pixelRect); s_savedBoxes.push_back(b);
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        case WM_KEYDOWN:
            if (wParam == VK_RETURN) {
                if (!s_isSnappingMode) {
                    s_isSnappingMode = true;
                    for (auto& mon : s_monitors) {
                        // Change transparency for snap mode
                        SetLayeredWindowAttributes(mon.hwndOverlay, 0, 150, LWA_ALPHA); 
                        // Move to bottom so it doesn't block focus, but use HTTRANSPARENT to allow clicks
                        SetWindowPos(mon.hwndOverlay, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
                        InvalidateRect(mon.hwndOverlay, NULL, TRUE);
                    }
                } else {
                    auto b = s_savedBoxes; HideOverlay(); if (s_onCompleted) s_onCompleted(b);
                }
            } else if (wParam == VK_ESCAPE) { HideOverlay(); if (s_onCancelled) s_onCancelled(); }
            return 0;
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}
