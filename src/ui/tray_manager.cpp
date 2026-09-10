#include "ui/tray_manager.hpp"
#include "../../resources/resource.h"
namespace biomes {
bool TrayManager::Add(HWND owner) {
    icon_ = {}; icon_.cbSize = sizeof(icon_); icon_.hWnd = owner; icon_.uID = 1;
    icon_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    icon_.uCallbackMessage = CallbackMessage;
    icon_.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_BIOMES));
    wcscpy_s(icon_.szTip, L"biomes");
    available_ = Shell_NotifyIconW(NIM_ADD, &icon_) != FALSE;
    if (available_) { icon_.uVersion = NOTIFYICON_VERSION_4; Shell_NotifyIconW(NIM_SETVERSION, &icon_); }
    return available_;
}
void TrayManager::Remove() { if (available_) Shell_NotifyIconW(NIM_DELETE, &icon_); available_ = false; }
UINT TrayManager::Menu(HWND owner, bool startup) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return 0;
    AppendMenuW(menu, MF_STRING, Open, L"Open Biomes");
    AppendMenuW(menu, MF_STRING | (startup ? MF_CHECKED : 0), Startup, L"Launch at Windows Startup");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, Exit, L"Exit Biomes");
    POINT point{}; GetCursorPos(&point); SetForegroundWindow(owner);
    const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, point.x, point.y, 0, owner, nullptr);
    DestroyMenu(menu); PostMessageW(owner, WM_NULL, 0, 0);
    return command;
}
}
