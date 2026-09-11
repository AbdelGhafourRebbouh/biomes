#include "core/json_manager.hpp"
#include "core/monitor_manager.hpp"
#include "core/window_scaler.hpp"
#include "core/app_paths.hpp"
#include "ui/webview_window.hpp"
#include "external/nlohmann/json.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <limits>

void WebViewWindow::SendMessageToUI(const std::string&) {}

int main() {
    try {
        auto require = [](bool value, const char* message) {
            if (!value) throw std::runtime_error(message);
        };
        const auto root = std::filesystem::temp_directory_path() /
            (L"biomes-layout-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        biomes::AppPaths::InitializeForTests(root);
        const auto path = biomes::AppPaths::BiomesFile();
        MonitorDetail left{}, right{};
        left.index = 0; left.stableId = "left"; left.deviceName = "DISPLAY1";
        left.rcWork = {-1920, 0, 0, 1040};
        right.index = 1; right.stableId = "right"; right.deviceName = "DISPLAY2";
        right.rcWork = {0, 0, 2560, 1400};
        WindowInfo window{};
        window.rect = {-100, 100, 800, 900};
        window.title = "Unicode \xc3\xa9 title";
        window.processPath = "C:\\Apps\\app.exe";
        window.processName = "app.exe";
        window.aumid = "Package!App";
        auto boxes = JsonManager::CaptureLayout({window}, {left, right});
        require(boxes.size() == 1 && boxes[0].stableMonitorId == "right", "largest intersection monitor");
        require(boxes[0].relX == 0 && boxes[0].relWidth == .3125f, "snapshot clipped to work area");
        window.rect = {3000, 0, 4000, 800};
        require(JsonManager::CaptureLayout({window}, {left, right}).empty(), "offscreen window skipped");
        BiomeProfile profile;
        profile.id = "fixture"; profile.name = "Layout"; profile.layout = boxes;
        profile.topologyHash = "two-displays";
        profile.layoutVariants["two-displays"] = boxes;
        profile.layoutVariants["empty-layout"] = {};
        require(JsonManager::SaveBiomesToFile(path, {profile}), "save isolated collection");
        std::vector<BiomeProfile> loaded;
        require(JsonManager::LoadBiomesFromFile(path, loaded) && loaded.size() == 1, "load collection");
        const auto& actual = loaded[0].layout[0];
        require(actual.titleHint == boxes[0].titleHint && actual.assignedApp == boxes[0].assignedApp &&
                actual.exeName == boxes[0].exeName && actual.aumid == boxes[0].aumid, "identity round trip");
        require(loaded[0].layoutVariants.size() == 2, "topology variants round trip");
        auto bad = profile; bad.layout[0].relWidth = std::numeric_limits<float>::quiet_NaN();
        require(!JsonManager::SaveBiomesToFile(path, {bad}), "invalid geometry rejected before write");
        require(JsonManager::LoadBiomesFromFile(path, loaded) && loaded[0].id == "fixture", "failed save preserves file");
        auto lockPath = path; lockPath += L".lock";
        HANDLE lock = CreateFileW(lockPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        require(lock != INVALID_HANDLE_VALUE, "hold writer lock");
        const bool lockedSave = JsonManager::SaveBiomesToFile(path, {profile});
        CloseHandle(lock);
        require(!lockedSave, "concurrent writer rejected");
        nlohmann::json data;
        { std::ifstream input(path); input >> data; }
        const auto original = data;
        data["version"] = 99;
        { std::ofstream output(path); output << data; }
        require(!JsonManager::LoadBiomesFromFile(path, loaded) && loaded[0].id == "fixture", "future schema preserves caller");
        data = original;
        data["biomes"].push_back({{"boxes", "malformed"}});
        { std::ofstream output(path); output << data; }
        require(!JsonManager::LoadBiomesFromFile(path, loaded) && loaded.size() == 1, "no partial load");
        profile.layout[0].stableMonitorId = "disconnected-fixture";
        profile.layout[0].monitorIndex = 0;
        JsonManager::EnrichProfileForSave(profile);
        require(profile.layout[0].stableMonitorId == "disconnected-fixture", "enrichment cannot remap missing monitor");
        profile.layoutVariants[MonitorManager::GetCurrentTopologyHash()] = {};
        require(JsonManager::SelectLayoutForTopology(profile).empty(), "empty topology variant remains authoritative");
        require(!GridOverlay::ShowOverlay(0, 14), "invalid overlay rows rejected");
        require(!GridOverlay::ShowOverlay(8, 129), "excessive overlay columns rejected");
        require(!GridOverlay::StartSnapping() && !GridOverlay::IsVisible(), "hidden overlay cannot snap");
        GridOverlay::HideOverlay();
        GridOverlay::HideOverlay();
        std::cout << "Layout regressions passed (isolated fixtures: " << root << ")\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
