#include "../../include/core/json_manager.hpp"
#include "../../include/external/nlohmann/json.hpp"
#include <fstream>
#include <iostream>
#include <set>
#include <filesystem>
#include <windows.h>

using json = nlohmann::json;

namespace {

json SerializeBiome(const BiomeProfile& profile) {
    json j;
    j["id"] = profile.id;
    j["name"] = profile.name;
    j["hotkey"] = profile.hotkey;
    j["coverImagePath"] = profile.coverImagePath;
    j["boxes"] = json::array();

    for (const auto& box : profile.layout) {
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
        boxObj["aumid"] = box.aumid;
        j["boxes"].push_back(boxObj);
    }

    return j;
}

BiomeProfile DeserializeBiome(const json& j) {
    BiomeProfile profile;
    profile.id = j.value("id", "");
    profile.name = j.value("name", "Untitled Biome");
    profile.hotkey = j.value("hotkey", "");
    profile.coverImagePath = j.value("coverImagePath", "");

    if (j.contains("boxes") && j["boxes"].is_array()) {
        for (const auto& item : j["boxes"]) {
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
            box.aumid = item.value("aumid", "");
            if (box.exeName.empty() && !box.assignedApp.empty()) {
                box.exeName = std::filesystem::path(box.assignedApp).filename().string();
            }
            profile.layout.push_back(box);
        }
    }

    return profile;
}

} // namespace

bool JsonManager::SaveBiomeToFile(const std::string& filePath, const BiomeProfile& profile) {
    std::ofstream file(filePath);
    if (!file.is_open()) {
        std::cerr << "[JSON] Failed to open " << filePath << " for writing." << std::endl;
        return false;
    }

    file << SerializeBiome(profile).dump(4);
    file.close();
    std::cout << "[JSON] Successfully saved biome profile '" << profile.name << "' to " << filePath << std::endl;
    return true;
}

bool JsonManager::LoadBiomeFromFile(const std::string& filePath, BiomeProfile& outProfile) {
    std::ifstream file(filePath);
    if (!file.is_open()) {
        std::cerr << "[JSON] Failed to open " << filePath << " for reading." << std::endl;
        return false;
    }

    json j;
    file >> j;
    file.close();

    outProfile = DeserializeBiome(j);

    return true;
}

bool JsonManager::SaveBiomesToFile(const std::string& filePath, const std::vector<BiomeProfile>& profiles) {
    json root;
    root["version"] = 2;
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
        return true; // No saved Biomes yet is a valid first-run state.
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

        std::set<std::string> uniqueApps;
        for (const auto& box : profile.layout) {
            if (!box.assignedApp.empty()) {
                uniqueApps.insert(std::filesystem::path(box.assignedApp).filename().string());
            }
        }
        card["apps"] = uniqueApps;
        cards.push_back(card);
    }

    return cards.dump();
}
