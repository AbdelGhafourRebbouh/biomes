#pragma once
#include <string>
#include <vector>
#include "../ui/grid_overlay.hpp"

// Structure representing a full user workspace profile
struct BiomeProfile {
    std::string name;             // Profile Name (e.g., "Coding", "Research")
    std::string hotkey;           // Keyboard shortcut combo (e.g., "CTRL+ALT+C")
    std::vector<SelectedBox> layout; // Target grid regions across all monitors
};

class JsonManager {
public:
    // Saves a complete Biome profile to a local JSON file
    static bool SaveBiomeToFile(const std::string& filePath, const BiomeProfile& profile);

    // Reads a saved JSON file and populates a BiomeProfile struct
    static bool LoadBiomeFromFile(const std::string& filePath, BiomeProfile& outProfile);
};