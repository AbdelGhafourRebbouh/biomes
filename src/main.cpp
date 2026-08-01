#include "../include/core/monitor_manager.hpp"
#include "../include/core/biome_manager.hpp"
#include "../include/core/window_scaler.hpp"
#include "../include/core/json_manager.hpp"
#include "../include/core/hotkey_manager.hpp"
#include <iostream>

void ExecuteBiomeProfile(const std::string& profilePath) {
    BiomeProfile loadedProfile;
    if (JsonManager::LoadBiomeFromFile(profilePath, loadedProfile)) {
        std::cout << "\n>>> HOTKEY TRIGGERED: Loading Biome '" << loadedProfile.name << "' <<<" << std::endl;

        for (auto& box : loadedProfile.layout) {
            MonitorDetail targetMon;
            if (MonitorManager::GetMonitorByName(box.monitorDeviceName, targetMon)) {
                box.x = targetMon.rect.left + static_cast<int>(box.relX * targetMon.width);
                box.y = targetMon.rect.top + static_cast<int>(box.relY * targetMon.height);
                box.width = static_cast<int>(box.relWidth * targetMon.width);
                box.height = static_cast<int>(box.relHeight * targetMon.height);
            }
        }

        BiomeManager::ApplyLayout(loadedProfile.layout);
    }
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "--- TESTING GLOBAL HOTKEY ENGINE ---" << std::endl;
    std::cout << "========================================\n" << std::endl;

    std::string profileFile = "coding_biome.json";

    // Parse "CTRL+ALT+C" and register with OS
    UINT modifiers = 0;
    UINT vkKey = 0;
    std::string hotkeyString = "CTRL+ALT+C";

    if (HotkeyManager::ParseHotkeyString(hotkeyString, modifiers, vkKey)) {
        int HOTKEY_ID = 1;
        if (HotkeyManager::RegisterGlobalHotkey(HOTKEY_ID, modifiers, vkKey)) {
            std::cout << "[SUCCESS] Global Hotkey '" << hotkeyString << "' registered with Windows!" << std::endl;
            std::cout << "Press " << hotkeyString << " anywhere on your computer to trigger Biome layout snapping.\n" << std::endl;
            std::cout << "Listening for hotkey... (Press Ctrl+C in terminal to stop)\n" << std::endl;

            // Windows Event Message Loop to listen for global OS hotkeys
            MSG msg = { 0 };
            while (GetMessage(&msg, NULL, 0, 0) != 0) {
                if (msg.message == WM_HOTKEY) {
                    if (msg.wParam == HOTKEY_ID) {
                        ExecuteBiomeProfile(profileFile);
                    }
                }
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }

            HotkeyManager::UnregisterGlobalHotkey(HOTKEY_ID);
        } else {
            std::cout << "[FAILED] Could not register hotkey with Windows OS." << std::endl;
        }
    }

    return 0;
}