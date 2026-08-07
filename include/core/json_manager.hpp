#pragma once
#include <string>
#include <vector>
#include "../ui/grid_overlay.hpp"

// Structure representing a full user workspace profile
struct BiomeProfile {
    std::string id;               // Stable identifier used by dashboard actions and hotkeys
    std::string name;             // Profile Name (e.g., "Coding", "Research")
    std::string hotkey;           // Keyboard shortcut combo (e.g., "CTRL+ALT+C")
    std::string coverImagePath;   // Optional local image displayed on the dashboard card
    std::vector<SelectedBox> layout; // Target grid regions across all monitors
};

class JsonManager {
public:
    // Saves a complete Biome profile to a local JSON file
    static bool SaveBiomeToFile(const std::string& filePath, const BiomeProfile& profile);

    // Reads a saved JSON file and populates a BiomeProfile struct
    static bool LoadBiomeFromFile(const std::string& filePath, BiomeProfile& outProfile);

    // Collection API used by the dashboard. A single file contains every saved Biome.
    static bool SaveBiomesToFile(const std::string& filePath, const std::vector<BiomeProfile>& profiles);
    static bool LoadBiomesFromFile(const std::string& filePath, std::vector<BiomeProfile>& outProfiles);

    // Compact dashboard-safe representation: id, name, hotkey, cover, and unique app names.
    static std::string LoadBiomesAsJsonString(const std::string& filePath);
};
