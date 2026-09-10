#define NOMINMAX
#include "../include/ui/launch_panel.hpp"
#include "../include/external/nlohmann/json.hpp"
#include <shellapi.h>
#include <filesystem>

std::wstring capturePath;

// Isolated visual harness: never loads saved biomes or starts personal apps.
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR arguments, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;
    LaunchPanel::Initialize(nullptr);
    const std::wstring mode = arguments;
    LaunchPanel::SetTheme(mode.find(L"dark") != std::wstring::npos ? "dark" : "light");
    nlohmann::json data = {{"action","LAUNCH_PROGRESS"},{"session",1},{"name","Creative space"},
        {"state",mode.find(L"success") != std::wstring::npos ? "success" : mode.find(L"partial") != std::wstring::npos ? "partial" : "opening"},
        {"ready",5},{"total",6},{"failed",1},{"waiting",0},{"skipped",0},
        {"items",nlohmann::json::array({{{"app","Affinity"},{"state","failed"},{"detail","Workspace window did not appear."}}})}};
    if (data["state"] == "success") { data["ready"] = 6; data["failed"] = 0; }
    if (mode.find(L"constrained") != std::wstring::npos) {
        data["state"] = "success"; data["ready"] = 6; data["failed"] = 0;
        data["items"] = nlohmann::json::array({{{"app","Affinity"},{"state","constrained"},{"detail","Opened with the app's size limits."}}});
    }
    if (data["state"] == "opening") { data["failed"] = 0; data["items"] = nlohmann::json::array(); }
    LaunchPanel::Update(data.dump());
    if (mode.find(L"capture") != std::wstring::npos) {
        capturePath = std::filesystem::absolute(L"panel-preview.png").wstring();
        SetTimer(nullptr, 0, 1800, [](HWND, UINT, UINT_PTR timer, DWORD) {
            KillTimer(nullptr, timer); LaunchPanel::CaptureTestPreview(capturePath);
        });
    }
    SetTimer(nullptr, 0, 120000, [](HWND, UINT, UINT_PTR timer, DWORD) { KillTimer(nullptr,timer); PostQuitMessage(0); });
    MSG message{};
    while (GetMessageW(&message,nullptr,0,0)>0) { TranslateMessage(&message); DispatchMessageW(&message); }
    LaunchPanel::Shutdown(); CoUninitialize(); return static_cast<int>(message.wParam);
}
