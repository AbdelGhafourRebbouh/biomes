#define NOMINMAX
#include <iostream>
#include <vector>
#include <string>
#include <algorithm>
#include "../include/ui/grid_overlay.hpp"
#include "../include/core/json_manager.hpp"
#include "../include/core/window_scaler.hpp"

// Filter out non-interactive OS background host windows
bool IsUserApp(const WindowInfo& win) {
    if (win.processName == "TextInputHost.exe") return false;
    if (win.processName == "ApplicationFrameHost.exe" && win.title.empty()) return false;
    if (win.processName == "explorer.exe" && win.title.empty()) return false;
    return true;
}

int main() {
    std::cout << "=========================================" << std::endl;
    std::cout << "   [BIOMES] GRID CREATION & TESTING MODE  " << std::endl;
    std::cout << "=========================================" << std::endl;
    std::cout << "-> Draw your grid boxes on screen." << std::endl;
    std::cout << "-> Press ESC when finished to save & snap windows!\n" << std::endl;

    // 1. Show Creation Overlay
    if (GridOverlay::ShowOverlay(8, 14)) {
        MSG msg = {};
        while (GetMessage(&msg, NULL, 0, 0)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    // 2. Retrieve Created Boxes
    std::vector<SelectedBox> createdBoxes = GridOverlay::GetSavedBoxes();
    std::cout << "\n[BIOMES] Captured " << createdBoxes.size() << " target grid regions." << std::endl;

    if (createdBoxes.empty()) {
        std::cout << "[BIOMES] No boxes were created. Exiting." << std::endl;
        return 0;
    }

    // 3. Save layout to coding_biome.json
    BiomeProfile profile;
    profile.name = "Coding Workspace";
    profile.hotkey = "CTRL+ALT+C";
    profile.layout = createdBoxes;

    JsonManager::SaveBiomeToFile("coding_biome.json", profile);

    // 4. Scan active user applications
    std::vector<WindowInfo> rawWindows = WindowScaler::GetActiveWindows();
    std::vector<WindowInfo> activeWindows;

    for (const auto& win : rawWindows) {
        if (IsUserApp(win)) {
            activeWindows.push_back(win);
        }
    }

    std::cout << "\n[BIOMES] Found " << activeWindows.size() << " valid user apps:" << std::endl;
    for (size_t i = 0; i < activeWindows.size(); ++i) {
        std::cout << "  [" << i + 1 << "] " << activeWindows[i].processName 
                  << " -> " << activeWindows[i].title << std::endl;
    }

    // 5. Snap active windows into target boxes
    std::cout << "\n[BIOMES] Snapping windows..." << std::endl;
    size_t snapCount = std::min(activeWindows.size(), profile.layout.size());

    for (size_t i = 0; i < snapCount; ++i) {
        std::cout << " -> Snapping [" << activeWindows[i].processName 
                  << "] into Box #" << profile.layout[i].id << "..." << std::endl;

        WindowScaler::SnapToBox(activeWindows[i].hwnd, profile.layout[i]);
    }

    std::cout << "\n[BIOMES] Done!" << std::endl;
    return 0;
}