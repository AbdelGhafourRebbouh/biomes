#include "core/background_host.hpp"
#include "core/single_instance.hpp"
namespace biomes {
constexpr UINT ExitRequest = WM_APP + 192;
bool BackgroundHost::Initialize(HINSTANCE instance, const std::wstring& windowClass, bool withTray) {
    withTray_ = withTray;
    WNDCLASSW wc{}; wc.hInstance = instance; wc.lpfnWndProc = Proc; wc.lpszClassName = windowClass.c_str();
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, windowClass.c_str(), L"biomes background", WS_POPUP,
        0, 0, 0, 0, nullptr, nullptr, instance, this);
    if (!hwnd_) return false;
    taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
    if (withTray_) tray_.Add(hwnd_);
    return true;
}
BackgroundHost::~BackgroundHost() { tray_.Remove(); if (hwnd_) DestroyWindow(hwnd_); }
void BackgroundHost::RequestExit() { if (hwnd_) PostMessageW(hwnd_, ExitRequest, 0, 0); }
LRESULT CALLBACK BackgroundHost::Proc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    auto self = reinterpret_cast<BackgroundHost*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<BackgroundHost*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(hwnd,message,wp,lp);
    if (message == WM_QUERYENDSESSION) return TRUE;
    if (message == ExitRequest || message == WM_CLOSE || (message == WM_ENDSESSION && wp)) {
        self->stopping_ = true; PostMessageW(hwnd, WM_NULL, 0, 0); return 0;
    }
    if (message == WM_NCDESTROY) { self->hwnd_ = nullptr; return DefWindowProcW(hwnd,message,wp,lp); }
    if (self->stopping_) return DefWindowProcW(hwnd,message,wp,lp);
    if (message == SingleInstance::ActivateMessage) {
        self->pending_.push_back([self] { if (self->open) self->open(); });
        PostMessageW(hwnd, WM_NULL, 0, 0); return 1;
    }
    if (message == WM_HOTKEY) {
        self->pending_.push_back([self,wp] { if (self->hotkey) self->hotkey(static_cast<int>(wp)); }); return 0;
    }
    if (message == WM_DISPLAYCHANGE || (message == WM_SETTINGCHANGE && wp == SPI_SETWORKAREA)) {
        self->pending_.push_back([self] { if (self->displayChanged) self->displayChanged(); });
        PostMessageW(hwnd, WM_NULL, 0, 0); return 0;
    }
    if (self->taskbarCreated_ && message == self->taskbarCreated_ && self->withTray_) {
        if (!self->tray_.Add(hwnd)) self->pending_.push_back([self] { if (self->open) self->open(); });
        return 0;
    }
    if (message == TrayManager::CallbackMessage) {
        const UINT event = LOWORD(lp);
        if (event == NIN_SELECT || event == NIN_KEYSELECT)
            self->pending_.push_back([self] { if (self->open) self->open(); });
        else if (event == WM_CONTEXTMENU) self->pending_.push_back([self] {
            const UINT command = self->tray_.Menu(self->hwnd_, self->startupEnabled && self->startupEnabled());
            if (command == TrayManager::Open && self->open) self->open();
            if (command == TrayManager::Startup && self->toggleStartup) self->toggleStartup();
            if (command == TrayManager::Exit) self->RequestExit();
        });
        return 0;
    }
    return DefWindowProcW(hwnd,message,wp,lp);
}
int BackgroundHost::Run() {
    MSG msg{}; int result = 0;
    while (!stopping_) {
        const BOOL received = GetMessageW(&msg, nullptr, 0, 0);
        if (received <= 0) { result = received < 0 ? 1 : static_cast<int>(msg.wParam); break; }
        TranslateMessage(&msg); DispatchMessageW(&msg);
        // Execute outside Win32/COM callbacks, never inside a nested message pump.
        while (!stopping_ && !pending_.empty()) {
            auto action = std::move(pending_.front()); pending_.pop_front();
            try { action(); } catch (...) { MessageBoxW(nullptr,L"biomes could not complete the background action.",L"biomes",MB_OK | MB_ICONERROR); }
        }
    }
    stopping_ = true; pending_.clear();
    try { if (shutdown) shutdown(); } catch (...) { result = 1; }
    tray_.Remove();
    if (hwnd_) DestroyWindow(hwnd_);
    return result;
}
}
