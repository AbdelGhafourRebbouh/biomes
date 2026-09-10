#pragma once
#include <windows.h>
#include <shellapi.h>
namespace biomes {
class TrayManager {
public:
    static constexpr UINT CallbackMessage = WM_APP + 191;
    enum Command : UINT { Open = 1, Startup = 2, Exit = 3 };
    bool Add(HWND owner);
    void Remove();
    UINT Menu(HWND owner, bool startup);
    bool Available() const { return available_; }
    ~TrayManager() { Remove(); }
private:
    NOTIFYICONDATAW icon_{};
    bool available_ = false;
};
}
