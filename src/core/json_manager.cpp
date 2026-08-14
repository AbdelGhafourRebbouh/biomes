#include "../../include/core/json_manager.hpp"
#include "../../include/core/monitor_manager.hpp"
#include "../../include/external/nlohmann/json.hpp"
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <filesystem>
#include <windows.h>

using json = nlohmann::json;

namespace {

json SerializeBox(const SelectedBox& box) {
    json boxObj;
    boxObj["id"] = box.id;
    boxObj["monitorIndex"] = box.monitorIndex;
    boxObj["startCol"] = box.startCol;
    boxObj["endCol"] = box.endCol;
    boxObj["startRow"] = box.startRow;
    boxObj["endRow"] = box.endRow;
    boxObj["relX"] = box.relX;
    boxObj["relY"] = box.relY;
    boxObj["relWidth"] = box.relWidth;
    boxObj["relHeight"] = box.relHeight;
    boxObj["assignedApp"] = box.assignedApp;
    boxObj["exeName"] = box.exeName;
    boxObj["titleHint"] = box.titleHint;
    boxObj["monitorDevice"] = box.monitorDevice;
    boxObj["stableMonitorId"] = box.stableMonitorId;
    boxObj["topologyHash"] = box.topologyHash;
    boxObj["aumid"] = box.aumid;
    boxObj["launchUri"] = box.launchUri;
    return boxObj;
}

SelectedBox DeserializeBox(const json& item) {
    SelectedBox box{};
    box.id = item.value("id", 0);
    box.monitorIndex = item.value("monitorIndex", 0);
    box.startCol = item.value("startCol", 0);
    box.endCol = item.value("endCol", 0);
    box.startRow = item.value("startRow", 0);
    box.endRow = item.value("endRow", 0);
    box.relX = item.value("relX", 0.0f);
    box.relY = item.value("relY", 0.0f);
    box.relWidth = item.value("relWidth", 0.0f);
    box.relHeight = item.value("relHeight", 0.0f);
    box.assignedApp = item.value("assignedApp", "");
    box.exeName = item.value("exeName", "");
    box.titleHint = item.value("titleHint", "");
    box.monitorDevice = item.value("monitorDevice", "");
    box.stableMonitorId = item.value("stableMonitorId", "");
    box.topologyHash = item.value("topologyHash", "");
    box.aumid = item.value("aumid", "");
    box.launchUri = item.value("launchUri", "");
    if (box.exeName.empty() && !box.assignedApp.empty()) {
        box.exeName = std::filesystem::path(box.assignedApp).filename().string();
    }
    return box;
}

json SerializeBiome(const BiomeProfile& profile) {
    json j;
    j["id"] = profile.id;
    j["name"] = profile.name;
    j["hotkey"] = profile.hotkey;
    j["coverImagePath"] = profile.coverImagePath;
    j["topologyHash"] = profile.topologyHash;
    j["boxes"] = json::array();
    for (const auto& box : profile.layout) {
        j["boxes"].push_back(SerializeBox(box));
    }

    if (!profile.layoutVariants.empty()) {
        json variants = json::object();
        for (const auto& entry : profile.layoutVariants) {
            json boxes = json::array();
            for (const auto& box : entry.second) {
                boxes.push_back(SerializeBox(box));
            }
            variants[entry.first] = boxes;
        }
        j["layoutVariants"] = variants;
    }

    return j;
}

BiomeProfile DeserializeBiome(const json& j) {
    BiomeProfile profile;
    profile.id = j.value("id", "");
    profile.name = j.value("name", "Untitled Biome");
    profile.hotkey = j.value("hotkey", "");
    profile.coverImagePath = j.value("coverImagePath", "");
    profile.topologyHash = j.value("topologyHash", "");

    if (j.contains("boxes") && j["boxes"].is_array()) {
        for (const auto& item : j["boxes"]) {
            profile.layout.push_back(DeserializeBox(item));
        }
    }

    if (j.contains("layoutVariants") && j["layoutVariants"].is_object()) {
        for (const auto& entry : j["layoutVariants"].items()) {
            std::vector<SelectedBox> variantBoxes;
            if (entry.value().is_array()) {
                for (const auto& item : entry.value()) {
                    variantBoxes.push_back(DeserializeBox(item));
                }
            }
            profile.layoutVariants[entry.key()] = std::move(variantBoxes);
        }
    }

    return profile;
}

MonitorBoxRef ToMonitorBoxRef(const SelectedBox& box) {
    MonitorBoxRef ref{};
    ref.stableMonitorId = box.stableMonitorId;
    ref.monitorDevice = box.monitorDevice;
    ref.monitorIndex = box.monitorIndex;
    return ref;
}

void EnrichBoxMonitorFields(SelectedBox& box) {
    const auto monitors = MonitorManager::GetConnectedMonitors();
    if (box.monitorIndex >= 0 && box.monitorIndex < static_cast<int>(monitors.size())) {
        const auto& mon = monitors[box.monitorIndex];
        if (box.monitorDevice.empty()) box.monitorDevice = mon.deviceName;
        if (box.stableMonitorId.empty()) box.stableMonitorId = mon.stableId;
    } else if (!box.stableMonitorId.empty()) {
        MonitorDetail detail;
        if (MonitorManager::GetMonitorByStableId(box.stableMonitorId, detail)) {
            box.monitorIndex = detail.index;
            box.monitorDevice = detail.deviceName;
        }
    } else if (!box.monitorDevice.empty()) {
        MonitorDetail detail;
        if (MonitorManager::GetMonitorByName(box.monitorDevice, detail)) {
            box.monitorIndex = detail.index;
            box.stableMonitorId = detail.stableId;
        }
    }

    box.topologyHash = MonitorManager::GetCurrentTopologyHash();
}

} // namespace

bool JsonManager::SaveBiomesToFile(const std::string& filePath, const std::vector<BiomeProfile>& profiles) {
    json root;
    root["version"] = 3;
    root["biomes"] = json::array();
    for (const auto& profile : profiles) {
        root["biomes"].push_back(SerializeBiome(profile));
    }

    const std::filesystem::path target(filePath);
    const std::filesystem::path temporary = target.string() + ".tmp";
    if (!target.parent_path().empty()) {
        std::error_code directoryError;
        std::filesystem::create_directories(target.parent_path(), directoryError);
        if (directoryError) return false;
    }

    std::ofstream file(temporary, std::ios::trunc);
    if (!file.is_open()) {
        std::cerr << "[JSON] Failed to open " << temporary << " for writing." << std::endl;
        return false;
    }
    file << root.dump(4);
    file.flush();
    if (!file.good()) return false;
    file.close();

    if (!MoveFileExA(temporary.string().c_str(), target.string().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::cerr << "[JSON] Failed to replace " << target << " (Win32 error " << GetLastError() << ")." << std::endl;
        return false;
    }
    return true;
}

bool JsonManager::LoadBiomesFromFile(const std::string& filePath, std::vector<BiomeProfile>& outProfiles) {
    outProfiles.clear();
    std::ifstream file(filePath);
    if (!file.is_open()) {
        return true;
    }

    try {
        json root;
        file >> root;
        if (!root.contains("biomes") || !root["biomes"].is_array()) {
            std::cerr << "[JSON] Invalid Biomes collection in " << filePath << std::endl;
            return false;
        }
        for (const auto& item : root["biomes"]) {
            outProfiles.push_back(DeserializeBiome(item));
        }
    } catch (const json::exception& error) {
        std::cerr << "[JSON] Failed to parse " << filePath << ": " << error.what() << std::endl;
        return false;
    }

    return true;
}

std::string JsonManager::LoadBiomesAsJsonString(const std::string& filePath) {
    std::vector<BiomeProfile> profiles;
    if (!LoadBiomesFromFile(filePath, profiles)) {
        return "[]";
    }

    json cards = json::array();
    for (const auto& profile : profiles) {
        json card;
        card["id"] = profile.id;
        card["name"] = profile.name;
        card["hotkey"] = profile.hotkey;
        card["cover"] = profile.coverImagePath;
        card["topologyHash"] = profile.topologyHash;
        card["hasLayoutVariants"] = !profile.layoutVariants.empty();

        std::set<std::string> uniqueApps;
        for (const auto& box : profile.layout) {
            if (!box.assignedApp.empty()) {
                uniqueApps.insert(std::filesystem::path(box.assignedApp).filename().string());
            }
        }
        card["apps"] = uniqueApps;

        std::vector<MonitorBoxRef> zoneRefs;
        zoneRefs.reserve(profile.layout.size());
        for (const auto& box : profile.layout) {
            zoneRefs.push_back(ToMonitorBoxRef(box));
        }
        const auto health = MonitorManager::EvaluateBiomeMonitorHealth(
            zoneRefs, profile.topologyHash);

        json monitorHealth;
        monitorHealth["connected"] = health.connectedMonitors;
        monitorHealth["required"] = health.requiredMonitors;
        monitorHealth["connectedRequired"] = health.connectedRequired;
        monitorHealth["missingZones"] = health.missingZones;
        monitorHealth["topologyMatch"] = health.topologyMatch;
        monitorHealth["warnings"] = health.warnings;
        card["monitorHealth"] = monitorHealth;

        cards.push_back(card);
    }

    return cards.dump();
}

void JsonManager::EnrichProfileForSave(BiomeProfile& profile) {
    const std::string currentHash = MonitorManager::GetCurrentTopologyHash();
    profile.topologyHash = currentHash;

    for (auto& box : profile.layout) {
        EnrichBoxMonitorFields(box);
    }

    profile.layoutVariants[currentHash] = profile.layout;
}

std::vector<SelectedBox> JsonManager::SelectLayoutForTopology(const BiomeProfile& profile) {
    const std::string currentHash = MonitorManager::GetCurrentTopologyHash();
    const auto variantIt = profile.layoutVariants.find(currentHash);
    if (variantIt != profile.layoutVariants.end() && !variantIt->second.empty()) {
        return variantIt->second;
    }
    return profile.layout;
}

std::vector<SelectedBox> JsonManager::RemapLayoutToCurrentMonitors(const std::vector<SelectedBox>& layout) {
    std::vector<SelectedBox> remapped = layout;
    for (auto& box : remapped) {
        MonitorBoxRef ref = ToMonitorBoxRef(box);
        const auto resolved = MonitorManager::ResolveMonitorForBox(ref);
        if (resolved.resolvedIndex >= 0) {
            MonitorDetail detail;
            const auto monitors = MonitorManager::GetConnectedMonitors();
            if (resolved.resolvedIndex < static_cast<int>(monitors.size())) {
                detail = monitors[resolved.resolvedIndex];
                box.monitorIndex = detail.index;
                box.monitorDevice = detail.deviceName;
                box.stableMonitorId = detail.stableId;
            }
        }
        box.topologyHash = MonitorManager::GetCurrentTopologyHash();
    }
    return remapped;
}
