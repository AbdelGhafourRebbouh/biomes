#include "../include/core/monitor_manager.hpp"
#include "../include/core/biome_manager.hpp"
#include "../include/core/window_scaler.hpp"
#include "../include/core/json_manager.hpp"
#include <iostream>

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "--- TESTING JSON SAVE & LOAD SYSTEM ---" << std::endl;
    std::cout << "========================================\n" << std::endl;

    auto monitors = MonitorManager::GetConnectedMonitors();
    if (monitors.size() < 2) {
        std::cout << "Requires 2 connected monitors for this test!" << std::endl;
        return 0;
    }

    // 1. Build Multi-Monitor Grid Layout
    auto mon0Layout = BiomeManager::GenerateWindowGridForMonitor(monitors[0], 1, 2, 15);
    auto mon1Layout = BiomeManager::GenerateWindowGridForMonitor(monitors[1], 1, 2, 15);

    mon1Layout[0].assignedAppPath = "Code.exe";
    mon0Layout[0].assignedAppPath = "Obsidian.exe";
    mon0Layout[1].assignedAppPath = "chrome.exe";

    BiomeProfile originalProfile;
    originalProfile.name = "Coding & Research";
    originalProfile.hotkey = "CTRL+ALT+C";
    originalProfile.layout.insert(originalProfile.layout.end(), mon0Layout.begin(), mon0Layout.end());
    originalProfile.layout.insert(originalProfile.layout.end(), mon1Layout.begin(), mon1Layout.end());

    // 2. Save profile to JSON file
    std::string filename = "coding_biome.json";
    if (JsonManager::SaveBiomeToFile(filename, originalProfile)) {
        std::cout << "[SUCCESS] Saved profile '" << originalProfile.name << "' to " << filename << std::endl;
    } else {
        std::cout << "[FAILED] Could not save JSON profile!" << std::endl;
        return 0;
    }

    // 3. Load profile from JSON file
    BiomeProfile loadedProfile;
    if (JsonManager::LoadBiomeFromFile(filename, loadedProfile)) {
        std::cout << "[SUCCESS] Loaded profile '" << loadedProfile.name 
                  << "' (Hotkey: " << loadedProfile.hotkey << ") from JSON!\n" << std::endl;

        // Convert stored percentage bounds back into pixel screen coordinates for target monitors
        for (auto& box : loadedProfile.layout) {
            MonitorDetail targetMon;
            if (MonitorManager::GetMonitorByName(box.monitorDeviceName, targetMon)) {
                box.x = targetMon.rect.left + static_cast<int>(box.relX * targetMon.width);
                box.y = targetMon.rect.top + static_cast<int>(box.relY * targetMon.height);
                box.width = static_cast<int>(box.relWidth * targetMon.width);
                box.height = static_cast<int>(box.relHeight * targetMon.height);
            }
        }

        std::cout << "Applying loaded JSON layout in 3 seconds..." << std::endl;
        Sleep(3000);

        BiomeManager::ApplyLayout(loadedProfile.layout);
    } else {
        std::cout << "[FAILED] Could not load JSON profile!" << std::endl;
    }

    return 0;
}