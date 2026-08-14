#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include "../ui/grid_overlay.hpp"

// Structure representing a full user workspace profile
struct BiomeProfile {
    std::string id;               // Stable identifier used by dashboard actions and hotkeys
    std::string name;             // Profile Name (e.g., "Coding", "Research")
    std::string hotkey;           // Keyboard shortcut combo (e.g., "CTRL+ALT+C")
    std::string coverImagePath;   // Optional local image displayed on the dashboard card
    std::string topologyHash;     // Monitor topology when layout was last saved
    std::vector<SelectedBox> layout; // Default layout for current/last topology
    std::unordered_map<std::string, std::vector<SelectedBox>> layoutVariants; // Per-topology layouts
};

class JsonManager {
public:
    // Collection API used by the dashboard. A single file contains every saved Biome.
    static bool SaveBiomesToFile(const std::string& filePath, const std::vector<BiomeProfile>& profiles);
    static bool LoadBiomesFromFile(const std::string& filePath, std::vector<BiomeProfile>& outProfiles);

    // Compact dashboard-safe representation: id, name, hotkey, cover, apps, monitor health.
    static std::string LoadBiomesAsJsonString(const std::string& filePath);

    // Fill stableMonitorId/topologyHash and store layout variant for current topology.
    static void EnrichProfileForSave(BiomeProfile& profile);

    // Pick layout variant for the connected monitor topology.
    static std::vector<SelectedBox> SelectLayoutForTopology(const BiomeProfile& profile);

    // Repair flow: remap saved zones onto currently connected monitors.
    static std::vector<SelectedBox> RemapLayoutToCurrentMonitors(const std::vector<SelectedBox>& layout);
};
