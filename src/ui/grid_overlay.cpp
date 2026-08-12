#define NOMINMAX
#include "../../include/ui/grid_overlay.hpp"

#include <windows.h>
#include <iostream>
#include <algorithm>
#include <filesystem>
#include <psapi.h>

// ---------------------------------------------------------------------------
// GridOverlay — fullscreen top-level popup(s), one per monitor.
//
// Must NOT be a child of the dashboard. Must cover rcMonitor edge-to-edge.
// DWM blur-behind on the whole layered window is avoided — it made the overlay
// look like a panel stuck on the Biomes window instead of a real fullscreen grid.
// Glass + corner radius apply ONLY to user-drawn boxes.
// ---------------------------------------------------------------------------

std::vector<MonitorInfoData> GridOverlay::s_monitors;
int GridOverlay::s_rows = 8;
int GridOverlay::s_cols = 14;
OverlayTheme GridOverlay::s_theme;
bool GridOverlay::s_isSnappingMode = false;
bool GridOverlay::s_isDragging = false;
POINT GridOverlay::s_dragStart = {0, 0}, GridOverlay::s_dragCurrent = {0, 0};
HWND GridOverlay::s_activeDragHwnd = nullptr, GridOverlay::s_movingWindowHwnd = nullptr;
HWINEVENTHOOK GridOverlay::s_hWinEventHook = nullptr;
std::vector<SelectedBox> GridOverlay::s_savedBoxes;
int GridOverlay::s_hoveredBoxId = -1;
std::function<void(const std::vector<SelectedBox>&)> GridOverlay::s_onCompleted;
std::function<void()> GridOverlay::s_onCancelled;

const int HOTKEY_ID = 999;
static const char* kOverlayClass = "BiomesGridOverlayFullscreen";

namespace {

HWND ResolveRootWindow(HWND hwnd) {
    HWND root = GetAncestor(hwnd, GA_ROOT);
    return root ? root : hwnd;
}

void BindWindowToBox(SelectedBox& box, HWND hwnd, const MonitorInfoData& monitor) {
    hwnd = ResolveRootWindow(hwnd);
    box.monitorDevice = monitor.deviceName;

    char title[512];
    GetWindowTextA(hwnd, title, sizeof(title));
    box.titleHint = title;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (hp) {
        char path[MAX_PATH];
        DWORD sz = MAX_PATH;
        if (QueryFullProcessImageNameA(hp, 0, path, &sz)) {
            box.assignedApp = path;
            box.exeName = std::filesystem::path(path).filename().string();
        }
        CloseHandle(hp);
    }
}

void DrawGlassBox(HDC hdc, RECT box, COLORREF fill, COLORREF border, int penWidth, int radius, bool hovered) {
    // Clean rounded glass card — no soft-shadow stacks (those pixelate and cut the grid).
    HBRUSH fillBrush = CreateSolidBrush(fill);
    HPEN borderPen = CreatePen(PS_SOLID, hovered ? penWidth + 1 : penWidth, border);
    HGDIOBJ oldB = SelectObject(hdc, fillBrush);
    HGDIOBJ oldP = SelectObject(hdc, borderPen);
    RoundRect(hdc, box.left, box.top, box.right, box.bottom, radius, radius);

    HPEN rimPen = CreatePen(PS_SOLID, 1, RGB(230, 235, 245));
    SelectObject(hdc, rimPen);
    SelectObject(hdc, GetStockObject(NULL_BRUSH));
    RoundRect(hdc, box.left + 1, box.top + 1, box.right - 1, box.bottom - 1,
              (std::max)(4, radius - 2), (std::max)(4, radius - 2));

    SelectObject(hdc, oldB);
    SelectObject(hdc, oldP);
    DeleteObject(fillBrush);
    DeleteObject(borderPen);
    DeleteObject(rimPen);
}

} // namespace

void GridOverlay::InvalidateAllOverlays() {
    for (auto& m : s_monitors) {
        if (m.hwndOverlay) InvalidateRect(m.hwndOverlay, nullptr, FALSE);
    }
}

SelectedBox* GridOverlay::HitTestBoxAtCursor(POINT screenPt) {
    for (auto& monitor : s_monitors) {
        if (!monitor.hwndOverlay) continue;
        POINT local = screenPt;
        if (!ScreenToClient(monitor.hwndOverlay, &local)) continue;

        SelectedBox* hit = nullptr;
        for (auto& box : s_savedBoxes) {
            if (box.monitorIndex != monitor.index) continue;
            if (PtInRect(&box.pixelRect, local)) hit = &box;
        }
        if (hit) return hit;
    }
    return nullptr;
}

void CALLBACK GridOverlay::WinEventProc(HWINEVENTHOOK, DWORD e, HWND hwnd, LONG obj, LONG child, DWORD, DWORD) {
    if (!s_isSnappingMode || obj != OBJID_WINDOW || child != CHILDID_SELF) return;

    if (e == EVENT_SYSTEM_MOVESIZESTART) {
        s_movingWindowHwnd = ResolveRootWindow(hwnd);
        s_hoveredBoxId = -1;
        for (auto& m : s_monitors) {
            if (!m.hwndOverlay) continue;
            SetWindowPos(m.hwndOverlay, HWND_TOPMOST,
                         m.rect.left, m.rect.top,
                         m.rect.right - m.rect.left, m.rect.bottom - m.rect.top,
                         SWP_SHOWWINDOW | SWP_NOACTIVATE);
        }
        InvalidateAllOverlays();
    } else if (e == EVENT_OBJECT_LOCATIONCHANGE) {
        if (s_movingWindowHwnd != ResolveRootWindow(hwnd)) return;
        POINT pt{};
        GetCursorPos(&pt);
        SelectedBox* hit = HitTestBoxAtCursor(pt);
        s_hoveredBoxId = hit ? hit->id : -1;
        InvalidateAllOverlays();
    } else if (e == EVENT_SYSTEM_MOVESIZEEND) {
        HWND root = ResolveRootWindow(hwnd);
        if (s_movingWindowHwnd != root) return;

        POINT pt{};
        GetCursorPos(&pt);
        if (SelectedBox* hit = HitTestBoxAtCursor(pt)) {
            const auto& monitor = s_monitors[hit->monitorIndex];
            RECT screenBox = hit->pixelRect;
            MapWindowPoints(monitor.hwndOverlay, nullptr, reinterpret_cast<POINT*>(&screenBox), 2);
            SetWindowPos(root, HWND_TOP,
                         screenBox.left, screenBox.top,
                         screenBox.right - screenBox.left,
                         screenBox.bottom - screenBox.top,
                         SWP_SHOWWINDOW);
            BindWindowToBox(*hit, root, monitor);
        }

        s_movingWindowHwnd = nullptr;
        s_hoveredBoxId = -1;
        InvalidateAllOverlays();
    }
}

BOOL CALLBACK GridOverlay::MonitorEnumProc(HMONITOR h, HDC, LPRECT, LPARAM) {
    MONITORINFOEXA monitorInfo = {};
    monitorInfo.cbSize = sizeof(MONITORINFOEXA);
    if (!GetMonitorInfoA(h, &monitorInfo)) return TRUE;

    MonitorInfoData info;
    info.index = static_cast<int>(s_monitors.size());
    info.hMonitor = h;
    info.rect = monitorInfo.rcMonitor; // full screen, edge-to-edge
    info.hwndOverlay = nullptr;
    info.deviceName = monitorInfo.szDevice;
    s_monitors.push_back(info);

    std::cout << "[OVERLAY] Monitor " << info.index << " " << info.deviceName
              << " LTRB " << info.rect.left << "," << info.rect.top << ","
              << info.rect.right << "," << info.rect.bottom << std::endl;
    return TRUE;
}

bool GridOverlay::ShowOverlay(int r, int c, const OverlayTheme& t) {
    // Tear down any previous overlay first.
    HideOverlay();

    s_rows = r;
    s_cols = c;
    s_theme = t;
    s_savedBoxes.clear();
    s_monitors.clear();
    s_isSnappingMode = false;
    s_isDragging = false;
    s_hoveredBoxId = -1;

    EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc, 0);
    if (s_monitors.empty()) {
        std::cerr << "[OVERLAY] No monitors found." << std::endl;
        return false;
    }

    HINSTANCE hInst = GetModuleHandle(nullptr);

    WNDCLASSA wc = {};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInst;
    wc.lpszClassName = kOverlayClass;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.style = CS_HREDRAW | CS_VREDRAW;
    RegisterClassA(&wc);

    s_hWinEventHook = SetWinEventHook(
        EVENT_SYSTEM_MOVESIZESTART, EVENT_SYSTEM_MOVESIZEEND,
        nullptr, WinEventProc, 0, 0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    for (auto& m : s_monitors) {
        const int width = m.rect.right - m.rect.left;
        const int height = m.rect.bottom - m.rect.top;

        // Top-level popup (parent = NULL). No TOOLWINDOW — that can break z-order.
        // No DWM blur-behind — that made it look clipped to the dashboard.
        m.hwndOverlay = CreateWindowExA(
            WS_EX_TOPMOST | WS_EX_LAYERED,
            kOverlayClass,
            "Biomes Fullscreen Overlay",
            WS_POPUP,
            m.rect.left,
            m.rect.top,
            width,
            height,
            nullptr,
            nullptr,
            hInst,
            nullptr);

        if (!m.hwndOverlay) {
            std::cerr << "[OVERLAY] CreateWindowEx failed (" << GetLastError() << ")" << std::endl;
            continue;
        }

        SetWindowLongPtr(m.hwndOverlay, GWLP_USERDATA, static_cast<LONG_PTR>(m.index));
        SetLayeredWindowAttributes(m.hwndOverlay, 0, s_theme.bgAlpha, LWA_ALPHA);

        // Enter must work even without focus — register immediately for draw mode.
        RegisterHotKey(m.hwndOverlay, HOTKEY_ID, 0, VK_RETURN);

        SetWindowPos(
            m.hwndOverlay,
            HWND_TOPMOST,
            m.rect.left,
            m.rect.top,
            width,
            height,
            SWP_SHOWWINDOW | SWP_FRAMECHANGED);

        ShowWindow(m.hwndOverlay, SW_SHOW);
        UpdateWindow(m.hwndOverlay);
    }

    // Focus the first overlay so WM_KEYDOWN Enter also works as a backup.
    if (!s_monitors.empty() && s_monitors[0].hwndOverlay) {
        SetForegroundWindow(s_monitors[0].hwndOverlay);
        SetFocus(s_monitors[0].hwndOverlay);
    }

    return true;
}

void GridOverlay::HideOverlay() {
    if (s_hWinEventHook) {
        UnhookWinEvent(s_hWinEventHook);
        s_hWinEventHook = nullptr;
    }
    for (auto& m : s_monitors) {
        if (m.hwndOverlay) {
            UnregisterHotKey(m.hwndOverlay, HOTKEY_ID);
            DestroyWindow(m.hwndOverlay);
            m.hwndOverlay = nullptr;
        }
    }
    s_monitors.clear();
    s_hoveredBoxId = -1;
    s_movingWindowHwnd = nullptr;
    s_isSnappingMode = false;
    s_isDragging = false;
}

void GridOverlay::SetCompletedCallback(std::function<void(const std::vector<SelectedBox>&)> cb) {
    s_onCompleted = std::move(cb);
}
void GridOverlay::SetCancelledCallback(std::function<void()> cb) {
    s_onCancelled = std::move(cb);
}

void GridOverlay::DrawGrid(HDC hdc, HWND, int mIdx) {
    RECT client{};
    if (!s_monitors.empty() && mIdx >= 0 && mIdx < static_cast<int>(s_monitors.size()) && s_monitors[mIdx].hwndOverlay) {
        GetClientRect(s_monitors[mIdx].hwndOverlay, &client);
    }
    if (client.right <= 0 || client.bottom <= 0) return;

    const float cellW = static_cast<float>(client.right) / s_cols;
    const float cellH = static_cast<float>(client.bottom) / s_rows;
    const int radius = (std::max)(8, s_theme.cornerRadius);

    // 1) Draw boxes first (slightly inset so they sit inside cells).
    for (const auto& box : s_savedBoxes) {
        if (box.monitorIndex != mIdx) continue;

        RECT br = box.pixelRect;
        InflateRect(&br, -4, -4);
        if (br.right - br.left < 20 || br.bottom - br.top < 20) continue;

        const bool hovered = s_isSnappingMode && s_movingWindowHwnd && box.id == s_hoveredBoxId;
        const COLORREF fill = hovered ? s_theme.boxHoverColor : RGB(55, 60, 72);
        const COLORREF border = hovered ? RGB(200, 255, 235) : RGB(210, 220, 235);
        DrawGlassBox(hdc, br, fill, border, s_theme.boxPenWidth, radius, hovered);

        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, s_theme.textColor);
        RECT textRect = br;
        InflateRect(&textRect, -10, -10);

        if (hovered) {
            DrawTextA(hdc, "Drop here", -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else if (!box.assignedApp.empty()) {
            const std::string label = box.exeName.empty()
                ? box.assignedApp.substr(box.assignedApp.find_last_of("\\/") + 1)
                : box.exeName;
            DrawTextA(hdc, label.c_str(), -1, &textRect,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        } else if (s_isSnappingMode) {
            SetTextColor(hdc, RGB(190, 200, 215));
            DrawTextA(hdc, "Empty zone", -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }

    if (!s_isSnappingMode && s_isDragging) {
        RECT drag{
            std::min(s_dragStart.x, s_dragCurrent.x),
            std::min(s_dragStart.y, s_dragCurrent.y),
            std::max(s_dragStart.x, s_dragCurrent.x),
            std::max(s_dragStart.y, s_dragCurrent.y)
        };
        InflateRect(&drag, -2, -2);
        if (drag.right - drag.left > 8 && drag.bottom - drag.top > 8) {
            DrawGlassBox(hdc, drag, RGB(55, 60, 72), RGB(210, 220, 235), 2, radius, false);
        }
    }

    // 2) Draw grid LAST so lines stay continuous and are not cut by box borders.
    if (!s_isSnappingMode) {
        HPEN gridPen = CreatePen(PS_SOLID, 1, s_theme.gridLineColor);
        HGDIOBJ oldPen = SelectObject(hdc, gridPen);
        HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        for (int i = 1; i < s_cols; ++i) {
            const int x = static_cast<int>(i * cellW);
            MoveToEx(hdc, x, 0, nullptr);
            LineTo(hdc, x, client.bottom);
        }
        for (int j = 1; j < s_rows; ++j) {
            const int y = static_cast<int>(j * cellH);
            MoveToEx(hdc, 0, y, nullptr);
            LineTo(hdc, client.right, y);
        }
        SelectObject(hdc, oldPen);
        SelectObject(hdc, oldBrush);
        DeleteObject(gridPen);
    }
}

LRESULT CALLBACK GridOverlay::WindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    const int mIdx = static_cast<int>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT r{};
            GetClientRect(hwnd, &r);

            HDC mem = CreateCompatibleDC(hdc);
            HBITMAP bmp = CreateCompatibleBitmap(hdc, (std::max)(1L, r.right), (std::max)(1L, r.bottom));
            HGDIOBJ oldBmp = SelectObject(mem, bmp);

            HBRUSH bg = CreateSolidBrush(RGB(16, 18, 26));
            FillRect(mem, &r, bg);
            DeleteObject(bg);

            DrawGrid(mem, hwnd, mIdx);
            BitBlt(hdc, 0, 0, r.right, r.bottom, mem, 0, 0, SRCCOPY);

            SelectObject(mem, oldBmp);
            DeleteObject(bmp);
            DeleteDC(mem);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;

        case WM_NCHITTEST:
            return s_isSnappingMode ? HTTRANSPARENT : HTCLIENT;

        case WM_LBUTTONDOWN:
            if (!s_isSnappingMode) {
                s_isDragging = true;
                s_dragStart = { LOWORD(lp), HIWORD(lp) };
                s_dragCurrent = s_dragStart;
                SetCapture(hwnd);
            }
            return 0;

        case WM_MOUSEMOVE:
            if (s_isDragging) {
                s_dragCurrent = { LOWORD(lp), HIWORD(lp) };
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        case WM_LBUTTONUP:
            if (s_isDragging) {
                s_isDragging = false;
                ReleaseCapture();
                RECT r{};
                GetClientRect(hwnd, &r);
                const float cw = static_cast<float>(r.right) / s_cols;
                const float ch = static_cast<float>(r.bottom) / s_rows;
                const int sc = static_cast<int>(std::min(s_dragStart.x, s_dragCurrent.x) / cw);
                const int ec = static_cast<int>(std::max(s_dragStart.x, s_dragCurrent.x) / cw) + 1;
                const int sr = static_cast<int>(std::min(s_dragStart.y, s_dragCurrent.y) / ch);
                const int er = static_cast<int>(std::max(s_dragStart.y, s_dragCurrent.y) / ch) + 1;

                SelectedBox b;
                b.id = static_cast<int>(s_savedBoxes.size()) + 1;
                b.monitorIndex = mIdx;
                b.pixelRect = {
                    static_cast<LONG>(sc * cw), static_cast<LONG>(sr * ch),
                    static_cast<LONG>(ec * cw), static_cast<LONG>(er * ch)
                };
                b.relX = static_cast<float>(b.pixelRect.left) / r.right;
                b.relY = static_cast<float>(b.pixelRect.top) / r.bottom;
                b.relWidth = static_cast<float>(b.pixelRect.right - b.pixelRect.left) / r.right;
                b.relHeight = static_cast<float>(b.pixelRect.bottom - b.pixelRect.top) / r.bottom;
                b.startCol = sc; b.endCol = ec; b.startRow = sr; b.endRow = er;
                if (mIdx >= 0 && mIdx < static_cast<int>(s_monitors.size())) {
                    b.monitorDevice = s_monitors[mIdx].deviceName;
                }
                s_savedBoxes.push_back(b);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        case WM_KEYDOWN:
            if (wp == VK_RETURN) {
                // Same path as hotkey — first Enter = snap mode, second = finish.
                PostMessage(hwnd, WM_HOTKEY, HOTKEY_ID, 0);
                return 0;
            }
            if (wp == VK_ESCAPE) {
                HideOverlay();
                if (s_onCancelled) s_onCancelled();
                return 0;
            }
            return 0;

        case WM_HOTKEY:
            if (wp != HOTKEY_ID) return 0;

            if (!s_isSnappingMode) {
                // First Enter → snap / assign mode (clicks pass through to apps).
                if (s_savedBoxes.empty()) {
                    // Require at least one zone before continuing.
                    return 0;
                }
                s_isSnappingMode = true;
                s_hoveredBoxId = -1;
                for (auto& m : s_monitors) {
                    if (!m.hwndOverlay) continue;
                    const LONG_PTR style = GetWindowLongPtr(m.hwndOverlay, GWL_EXSTYLE);
                    SetWindowLongPtr(m.hwndOverlay, GWL_EXSTYLE,
                                     style | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE);
                    SetWindowPos(m.hwndOverlay, HWND_TOPMOST,
                                 m.rect.left, m.rect.top,
                                 m.rect.right - m.rect.left, m.rect.bottom - m.rect.top,
                                 SWP_SHOWWINDOW | SWP_NOACTIVATE | SWP_FRAMECHANGED);
                    InvalidateRect(m.hwndOverlay, nullptr, TRUE);
                }
                std::cout << "[OVERLAY] Enter → snap mode (" << s_savedBoxes.size() << " zones)" << std::endl;
                return 0;
            }

            // Second Enter → finish and return to dashboard.
            {
                auto boxes = s_savedBoxes;
                std::cout << "[OVERLAY] Enter → complete layout" << std::endl;
                HideOverlay();
                if (s_onCompleted) s_onCompleted(boxes);
            }
            return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}
