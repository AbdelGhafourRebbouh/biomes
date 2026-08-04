#include "../../include/core/json_manager.hpp"
#include "../../include/external/nlohmann/json.hpp"
#include <fstream>
#include <iostream>

using json = nlohmann::json;

bool JsonManager::SaveBiomeToFile(const std::string& filePath, const BiomeProfile& profile) {
    json j;
    j["name"] = profile.name;
    j["hotkey"] = profile.hotkey;
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

        j["boxes"].push_back(boxObj);
    }

    std::ofstream file(filePath);
    if (!file.is_open()) {
        std::cerr << "[JSON] Failed to open " << filePath << " for writing." << std::endl;
        return false;
    }

    file << j.dump(4);
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

    outProfile.name = j.value("name", "Untitled Biome");
    outProfile.hotkey = j.value("hotkey", "");
    outProfile.layout.clear();

    if (j.contains("boxes") && j["boxes"].is_array()) {
        for (const auto& item : j["boxes"]) {
            SelectedBox box;
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

            outProfile.layout.push_back(box);
        }
    }

    return true;
}