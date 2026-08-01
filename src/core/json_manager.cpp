#include "../../include/core/json_manager.hpp"
#include "../../include/external/nlohmann/json.hpp"
#include <fstream>
#include <iostream>
using namespace std;
using json = nlohmann::json;

bool JsonManager::SaveBiomeToFile(const string& filePath, const BiomeProfile& profile) {
    try {
        json j;
        j["biome_name"] = profile.name;
        j["hotkey"] = profile.hotkey;

        json boxesArray = json::array();
        for (const auto& box : profile.layout) {
            json boxJson;
            boxJson["id"] = box.id;
            boxJson["monitor_device"] = box.monitorDeviceName;
            
            // Normalized percentages for resolution-independent cross-device saving
            boxJson["rel_x"] = box.relX;
            boxJson["rel_y"] = box.relY;
            boxJson["rel_width"] = box.relWidth;
            boxJson["rel_height"] = box.relHeight;

            // Target application binaries / titles
            boxJson["assigned_app_path"] = box.assignedAppPath;
            boxJson["assigned_app_title"] = box.assignedAppTitle;

            boxesArray.push_back(boxJson);
        }

        j["boxes"] = boxesArray;

        ofstream file(filePath);
        if (!file.is_open()) return false;

        file << j.dump(4); // Formatted JSON with 4-space indentation
        file.close();
        return true;
    } 
    catch (const exception& e) {
        cerr << "JSON Save Error: " << e.what() << endl;
        return false;
    }
}

bool JsonManager::LoadBiomeFromFile(const string& filePath, BiomeProfile& outProfile) {
    try {
        ifstream file(filePath);
        if (!file.is_open()) return false;

        json j;
        file >> j;
        file.close();

        outProfile.name = j.value("biome_name", "Untitled Biome");
        outProfile.hotkey = j.value("hotkey", "");
        outProfile.layout.clear();

        if (j.contains("boxes") && j["boxes"].is_array()) {
            for (const auto& boxJson : j["boxes"]) {
                GridBox box;
                box.id = boxJson.value("id", 0);
                box.monitorDeviceName = boxJson.value("monitor_device", "");

                // Relative percentage bounds
                box.relX = boxJson.value("rel_x", 0.0f);
                box.relY = boxJson.value("rel_y", 0.0f);
                box.relWidth = boxJson.value("rel_width", 0.0f);
                box.relHeight = boxJson.value("rel_height", 0.0f);

                // Target apps
                box.assignedAppPath = boxJson.value("assigned_app_path", "");
                box.assignedAppTitle = boxJson.value("assigned_app_title", "");

                outProfile.layout.push_back(box);
            }
        }

        return true;
    } 
    catch (const exception& e) {
        cerr << "JSON Load Error: " << e.what() << endl;
        return false;
    }
}