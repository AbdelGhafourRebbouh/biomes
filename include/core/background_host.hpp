#pragma once
#include <windows.h>
#include <functional>
#include <deque>
#include <string>
#include "ui/tray_manager.hpp"
namespace biomes {
class BackgroundHost {
public:
    std::function<void()> open, shutdown, displayChanged, toggleStartup;
    std::function<bool()> startupEnabled;
    std::function<void()> checkUpdates, periodic;
    std::function<bool()> updateAvailable;
    void NotifyUpdate() { tray_.NotifyUpdate(); }
    std::function<void(int)> hotkey;
    bool Initialize(HINSTANCE instance, const std::wstring& windowClass, bool withTray = true);
    HWND Hwnd() const { return hwnd_; }
    bool CanHide() const { return tray_.Available(); }
    bool Stopping() const { return stopping_; }
    void RequestExit();
    void NotifyDisplayChange();
    int Run();
    ~BackgroundHost();
private:
    HWND hwnd_ = nullptr;
    TrayManager tray_;
    UINT taskbarCreated_ = 0;
    bool stopping_ = false, withTray_ = true;
    bool displayQueued_ = false;
    std::deque<std::function<void()>> pending_;
    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
};
}
