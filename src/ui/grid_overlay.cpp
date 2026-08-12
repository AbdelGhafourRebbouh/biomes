#define NOMINMAX
#include "../../include/ui/grid_overlay.hpp"
#include <windows.h>
#include <iostream>
#include <algorithm>

std::vector<MonitorInfoData> GridOverlay::s_monitors;
int GridOverlay::s_rows = 8;
int GridOverlay::s_cols = 14;
OverlayTheme GridOverlay::s_theme;
bool GridOverlay::s_isSnappingMode = false;
bool GridOverlay::s_isDragging = false;
POINT GridOverlay::s_dragStart = {0,0}, GridOverlay::s_dragCurrent = {0,0};
HWND GridOverlay::s_activeDragHwnd = NULL, GridOverlay::s_movingWindowHwnd = NULL;
HWINEVENTHOOK GridOverlay::s_hWinEventHook = NULL;
std::vector<SelectedBox> GridOverlay::s_savedBoxes;
std::function<void(const std::vector<SelectedBox>&)> GridOverlay::s_onCompleted;
std::function<void()> GridOverlay::s_onCancelled;

const int HOTKEY_ID = 999;

void CALLBACK GridOverlay::WinEventProc(HWINEVENTHOOK h, DWORD e, HWND hwnd, LONG obj, LONG child, DWORD t, DWORD time) {
    if (!s_isSnappingMode || obj != OBJID_WINDOW || child != CHILDID_SELF) return;
    if (e == EVENT_SYSTEM_MOVESIZESTART) {
        s_movingWindowHwnd = hwnd;
        for (auto& m : s_monitors) {
            SetWindowPos(m.hwndOverlay, HWND_TOPMOST, 0,0,0,0, SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
            InvalidateRect(m.hwndOverlay, NULL, FALSE);
        }
    } else if (e == EVENT_OBJECT_LOCATIONCHANGE) {
        if (s_movingWindowHwnd == hwnd) for (auto& m : s_monitors) InvalidateRect(m.hwndOverlay, NULL, FALSE);
    } else if (e == EVENT_SYSTEM_MOVESIZEEND) {
        if (s_movingWindowHwnd == hwnd) {
            POINT pt; GetCursorPos(&pt);
            for (auto& b : s_savedBoxes) {
                RECT r = b.pixelRect; OffsetRect(&r, s_monitors[b.monitorIndex].rect.left, s_monitors[b.monitorIndex].rect.top);
                if (PtInRect(&r, pt)) {
                    SetWindowPos(hwnd, HWND_TOP, r.left, r.top, r.right-r.left, r.bottom-r.top, SWP_SHOWWINDOW);
                    DWORD pid; GetWindowThreadProcessId(hwnd, &pid);
                    HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
                    if (hp) { char path[MAX_PATH]; DWORD sz=MAX_PATH; if (QueryFullProcessImageNameA(hp,0,path,&sz)) b.assignedApp = path; CloseHandle(hp); }
                    break;
                }
            }
            s_movingWindowHwnd = NULL;
            for (auto& m : s_monitors) InvalidateRect(m.hwndOverlay, NULL, FALSE);
        }
    }
}

BOOL CALLBACK GridOverlay::MonitorEnumProc(HMONITOR h, HDC hdc, LPRECT r, LPARAM d) {
    MONITORINFO monitorInfo = { sizeof(MONITORINFO) };
    if (!GetMonitorInfoA(h, &monitorInfo)) return TRUE;

    MonitorInfoData info;
    info.index = static_cast<int>(s_monitors.size());
    info.hMonitor = h;
    info.rect = monitorInfo.rcWork;
    info.hwndOverlay = NULL;
    s_monitors.push_back(info); return TRUE;
}

bool GridOverlay::ShowOverlay(int r, int c, const OverlayTheme& t) {
    s_rows = r; s_cols = c; s_theme = t; s_savedBoxes.clear(); s_monitors.clear(); s_isSnappingMode = false;
    EnumDisplayMonitors(NULL, NULL, MonitorEnumProc, 0);
    HINSTANCE hInst = GetModuleHandle(NULL);
    WNDCLASSA wc = {}; wc.lpfnWndProc = WindowProc; wc.hInstance = hInst; wc.lpszClassName = "BiomesGrid"; wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);
    s_hWinEventHook = SetWinEventHook(EVENT_SYSTEM_MOVESIZESTART, EVENT_SYSTEM_MOVESIZEEND, NULL, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    for (auto& m : s_monitors) {
        m.hwndOverlay = CreateWindowExA(WS_EX_TOPMOST|WS_EX_LAYERED|WS_EX_TOOLWINDOW, "BiomesGrid", "Overlay", WS_POPUP|WS_VISIBLE, m.rect.left, m.rect.top, m.rect.right-m.rect.left, m.rect.bottom-m.rect.top, NULL, NULL, hInst, NULL);
        SetLayeredWindowAttributes(m.hwndOverlay, 0, s_theme.bgAlpha, LWA_ALPHA);
        SetWindowLongPtr(m.hwndOverlay, GWLP_USERDATA, m.index);
    }
    return true;
}

void GridOverlay::HideOverlay() {
    if (s_hWinEventHook) { UnhookWinEvent(s_hWinEventHook); s_hWinEventHook = NULL; }
    for (auto& m : s_monitors) if (m.hwndOverlay) { UnregisterHotKey(m.hwndOverlay, HOTKEY_ID); DestroyWindow(m.hwndOverlay); }
    s_monitors.clear();
}

void GridOverlay::SetCompletedCallback(std::function<void(const std::vector<SelectedBox>&)> cb) { s_onCompleted = std::move(cb); }
void GridOverlay::SetCancelledCallback(std::function<void()> cb) { s_onCancelled = std::move(cb); }

void GridOverlay::DrawGrid(HDC hdc, HWND hwnd, int mIdx) {
    RECT r; GetClientRect(hwnd, &r);
    float cw = (float)r.right / s_cols, ch = (float)r.bottom / s_rows;
    HPEN p = CreatePen(PS_SOLID, 1, s_isSnappingMode ? RGB(60,60,60) : s_theme.gridLineColor);
    SelectObject(hdc, p);
    for (int i=1; i<s_cols; ++i) { MoveToEx(hdc, (int)(i*cw), 0, NULL); LineTo(hdc, (int)(i*cw), r.bottom); }
    for (int j=1; j<s_rows; ++j) { MoveToEx(hdc, 0, (int)(j*ch), NULL); LineTo(hdc, r.right, (int)(j*ch)); }
    DeleteObject(p);
    HBRUSH bB = CreateSolidBrush(s_theme.boxFillColor), hB = CreateSolidBrush(s_theme.boxHoverColor);
    HPEN bP = CreatePen(PS_SOLID, s_theme.boxPenWidth, s_theme.boxBorderColor); SelectObject(hdc, bP);
    POINT m; GetCursorPos(&m);
    for (auto& b : s_savedBoxes) {
        if (b.monitorIndex != mIdx) continue;
        RECT br = b.pixelRect; RECT sbr = br; OffsetRect(&sbr, s_monitors[mIdx].rect.left, s_monitors[mIdx].rect.top);
        bool ih = (s_isSnappingMode && s_movingWindowHwnd && PtInRect(&sbr, m));
        SelectObject(hdc, ih ? hB : bB);
        if (s_isSnappingMode && !ih) {
             HPEN dashPen = CreatePen(PS_DOT, 1, RGB(100,255,200)); SelectObject(hdc, dashPen);
             Rectangle(hdc, br.left, br.top, br.right, br.bottom); DeleteObject(dashPen);
        } else Rectangle(hdc, br.left, br.top, br.right, br.bottom);
        if (!b.assignedApp.empty()) { std::string n = b.assignedApp.substr(b.assignedApp.find_last_of("\\/") + 1); SetTextColor(hdc, RGB(255, 255, 255)); SetBkMode(hdc, TRANSPARENT); DrawTextA(hdc, n.c_str(), -1, &br, DT_CENTER|DT_VCENTER|DT_SINGLELINE); }
    }
    if (!s_isSnappingMode && s_isDragging) Rectangle(hdc, std::min(s_dragStart.x, s_dragCurrent.x), std::min(s_dragStart.y, s_dragCurrent.y), std::max(s_dragStart.x, s_dragCurrent.x), std::max(s_dragStart.y, s_dragCurrent.y));
    DeleteObject(bB); DeleteObject(hB); DeleteObject(bP);
}

LRESULT CALLBACK GridOverlay::WindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    int mIdx = (int)GetWindowLongPtr(hwnd, GWLP_USERDATA);
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps); RECT r; GetClientRect(hwnd, &r);
            HDC mdc = CreateCompatibleDC(hdc); HBITMAP mb = CreateCompatibleBitmap(hdc, r.right, r.bottom); SelectObject(mdc, mb);
            HBRUSH bg = CreateSolidBrush(s_isSnappingMode ? RGB(0,0,0) : RGB(20,20,20)); FillRect(mdc, &r, bg); DeleteObject(bg);
            DrawGrid(mdc, hwnd, mIdx); BitBlt(hdc, 0, 0, r.right, r.bottom, mdc, 0, 0, SRCCOPY);
            DeleteObject(mb); DeleteDC(mdc); EndPaint(hwnd, &ps); return 0;
        }
        case WM_NCHITTEST: return s_isSnappingMode ? HTTRANSPARENT : HTCLIENT;
        case WM_LBUTTONDOWN: if (!s_isSnappingMode) { s_isDragging = true; s_dragStart = {LOWORD(lp), HIWORD(lp)}; s_dragCurrent = s_dragStart; SetCapture(hwnd); } return 0;
        case WM_MOUSEMOVE: if (s_isDragging) { s_dragCurrent = {LOWORD(lp), HIWORD(lp)}; InvalidateRect(hwnd, NULL, FALSE); } return 0;
        case WM_LBUTTONUP: if (s_isDragging) {
            s_isDragging = false; ReleaseCapture(); RECT r; GetClientRect(hwnd, &r);
            float cw = (float)r.right/s_cols, ch = (float)r.bottom/s_rows;
            int sc = (int)(std::min(s_dragStart.x, s_dragCurrent.x)/cw), ec = (int)(std::max(s_dragStart.x, s_dragCurrent.x)/cw)+1, sr = (int)(std::min(s_dragStart.y, s_dragCurrent.y)/ch), er = (int)(std::max(s_dragStart.y, s_dragCurrent.y)/ch)+1;
            SelectedBox b; b.id = (int)s_savedBoxes.size()+1; b.monitorIndex = mIdx; b.pixelRect = {(int)(sc*cw), (int)(sr*ch), (int)(ec*cw), (int)(er*ch)};
            b.relX = (float)b.pixelRect.left/r.right; b.relY = (float)b.pixelRect.top/r.bottom; b.relWidth = (float)(b.pixelRect.right-b.pixelRect.left)/r.right; b.relHeight = (float)(b.pixelRect.bottom-b.pixelRect.top)/r.bottom;
            s_savedBoxes.push_back(b); InvalidateRect(hwnd, NULL, FALSE);
        } return 0;
        case WM_KEYDOWN: if (wp == VK_RETURN && !s_isSnappingMode) {
            s_isSnappingMode = true;
            for (auto& m : s_monitors) {
                RegisterHotKey(m.hwndOverlay, HOTKEY_ID, 0, VK_RETURN);
                const LONG_PTR style = GetWindowLongPtr(m.hwndOverlay, GWL_EXSTYLE);
                SetWindowLongPtr(m.hwndOverlay, GWL_EXSTYLE, style | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE);
                SetWindowPos(m.hwndOverlay, HWND_TOPMOST, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
                InvalidateRect(m.hwndOverlay, NULL, TRUE);
            }
            return 0;
        } else if (wp == VK_ESCAPE) { HideOverlay(); if (s_onCancelled) s_onCancelled(); return 0; }
        case WM_HOTKEY: if (wp == HOTKEY_ID && s_isSnappingMode) { auto b = s_savedBoxes; HideOverlay(); if (s_onCompleted) s_onCompleted(b); } return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}
