#include "../../include/core/json_manager.hpp"
#include "../../include/core/monitor_manager.hpp"
#include "../../include/external/nlohmann/json.hpp"
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <filesystem>
#include <windows.h>
#include <cmath>
#include <stdexcept>
#include "../../include/core/window_scaler.hpp"

using json = nlohmann::json;

namespace {

void ValidateBox(const SelectedBox& box) {
    if (!std::isfinite(box.relX) || !std::isfinite(box.relY) ||
        !std::isfinite(box.relWidth) || !std::isfinite(box.relHeight) ||
        box.relX < 0 || box.relY < 0 || box.relX >= 1 || box.relY >= 1 ||
        box.relWidth <= 0 || box.relHeight <= 0 ||
        box.relX + box.relWidth > 1.0001f || box.relY + box.relHeight > 1.0001f)
        throw std::runtime_error("Invalid relative layout bounds");
}

json SerializeBox(const SelectedBox& box) {
    ValidateBox(box);
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
    ValidateBox(box);
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
    if (!j.is_object() || !j.contains("boxes") || !j["boxes"].is_array())
        throw std::runtime_error("Invalid workspace layout");
    if (j.contains("layoutVariants") && !j["layoutVariants"].is_object())
        throw std::runtime_error("Invalid layout variants");
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
            if (!entry.value().is_array()) throw std::runtime_error("Invalid topology layout");
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

void EnrichBoxMonitorFields(SelectedBox& box, const std::vector<MonitorDetail>& monitors,
                            const std::string& topology) {
    const auto resolved = MonitorManager::ResolveMonitorForBox(ToMonitorBoxRef(box), monitors);
    for (const auto& monitor : monitors) {
        if (resolved.resolvedIndex < 0 || monitor.index != resolved.resolvedIndex) continue;
        box.monitorIndex = monitor.index;
        box.monitorDevice = monitor.deviceName;
        box.stableMonitorId = monitor.stableId;
        break;
    }
    box.topologyHash = topology;
}

} // namespace

bool JsonManager::SaveBiomesToFile(const std::filesystem::path& filePath, const std::vector<BiomeProfile>& profiles) {
    try {
    json root;
    root["version"] = 3;
    root["biomes"] = json::array();
    for (const auto& profile : profiles) {
        root["biomes"].push_back(SerializeBiome(profile));
    }

    const std::filesystem::path target(filePath);
    std::filesystem::path temporary = target;
    temporary += L".tmp";
    if (!target.parent_path().empty()) {
        std::error_code directoryError;
        std::filesystem::create_directories(target.parent_path(), directoryError);
        if (directoryError) return false;
    }

    const std::string bytes = root.dump(4);
    // Serialize writers before touching the shared staging file.
    auto lockPath = target; lockPath += L".lock";
    HANDLE lock = CreateFileW(lockPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (lock == INVALID_HANDLE_VALUE) return false;
    struct HandleGuard { HANDLE h; ~HandleGuard() { CloseHandle(h); } } lockGuard{lock};
    HANDLE output = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool saved = bytes.size() <= MAXDWORD &&
        WriteFile(output, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
        written == bytes.size() && FlushFileBuffers(output);
    const bool closed = CloseHandle(output) != FALSE;
    if (!saved || !closed) return false;
    if (!MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return false;
    return true;
    } catch (const std::exception&) { return false; }
}

bool JsonManager::LoadBiomesFromFile(const std::filesystem::path& filePath, std::vector<BiomeProfile>& outProfiles) {
    std::vector<BiomeProfile> loaded;
    std::ifstream file(filePath);
    if (!file.is_open()) {
        std::error_code error;
        const bool exists = std::filesystem::exists(filePath, error);
        if (!exists && !error) { outProfiles.clear(); return true; }
        return false;
    }

    try {
        json root;
        file >> root;
        if (!root.is_object() || (root.contains("version") &&
            (!root["version"].is_number_integer() || root["version"] < 1 || root["version"] > 3)))
            return false;
        if (!root.contains("biomes") || !root["biomes"].is_array()) {
            std::cerr << "[JSON] Invalid Biomes collection in " << filePath << std::endl;
            return false;
        }
        for (const auto& item : root["biomes"]) {
            loaded.push_back(DeserializeBiome(item));
        }
    } catch (const std::exception& error) {
        std::cerr << "[JSON] Failed to parse " << filePath << ": " << error.what() << std::endl;
        return false;
    }

    outProfiles = std::move(loaded);
    return true;
}

std::string JsonManager::LoadBiomesAsJsonString(const std::filesystem::path& filePath) {
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
        // Count unique screens used by the same topology-selected layout as activation.
        // connectedRequired above counts resolved zones, not screens.
        std::set<int> openingScreens;
        for (const auto& box : SelectLayoutForTopology(profile)) {
            const auto resolved = MonitorManager::ResolveMonitorForBox(ToMonitorBoxRef(box));
            if (resolved.resolvedIndex >= 0) openingScreens.insert(resolved.resolvedIndex);
        }
        monitorHealth["openingScreens"] = openingScreens.size();
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
    const auto monitors = MonitorManager::GetConnectedMonitors();
    const std::string currentHash = MonitorManager::GetTopologyHash(monitors);
    profile.topologyHash = currentHash;

    for (auto& box : profile.layout) {
        EnrichBoxMonitorFields(box, monitors, currentHash);
    }

    profile.layoutVariants[currentHash] = profile.layout;
}

std::vector<SelectedBox> JsonManager::SelectLayoutForTopology(const BiomeProfile& profile) {
    const std::string currentHash = MonitorManager::GetCurrentTopologyHash();
    const auto variantIt = profile.layoutVariants.find(currentHash);
    if (variantIt != profile.layoutVariants.end()) {
        return variantIt->second;
    }
    return profile.layout;
}

std::vector<SelectedBox> JsonManager::RemapLayoutToCurrentMonitors(const std::vector<SelectedBox>& layout) {
    std::vector<SelectedBox> remapped = layout;
    const auto monitors = MonitorManager::GetConnectedMonitors();
    const auto topology = MonitorManager::GetTopologyHash(monitors);
    for (auto& box : remapped) EnrichBoxMonitorFields(box, monitors, topology);
    return remapped;
}

std::vector<SelectedBox> JsonManager::CaptureLayout(const std::vector<WindowInfo>& windows,
                                                   const std::vector<MonitorDetail>& monitors) {
    std::vector<SelectedBox> boxes;
    const auto topology = MonitorManager::GetTopologyHash(monitors);
    for (const auto& window : windows) {
        const MonitorDetail* best = nullptr;
        RECT clipped{};
        long long largest = 0;
        for (const auto& monitor : monitors) {
            RECT intersection{};
            if (!IntersectRect(&intersection, &window.rect, &monitor.rcWork)) continue;
            const long long area = (static_cast<long long>(intersection.right) - intersection.left) *
                                   (static_cast<long long>(intersection.bottom) - intersection.top);
            if (area > largest) { largest = area; best = &monitor; clipped = intersection; }
        }
        if (!best || (window.processPath.empty() && window.processName.empty() && window.aumid.empty())) continue;
        SelectedBox box;
        box.id = static_cast<int>(boxes.size()) + 1;
        box.monitorIndex = best->index;
        box.monitorDevice = best->deviceName;
        box.stableMonitorId = best->stableId;
        box.topologyHash = topology;
        const double width = static_cast<double>(best->rcWork.right) - best->rcWork.left;
        const double height = static_cast<double>(best->rcWork.bottom) - best->rcWork.top;
        box.relX = static_cast<float>((clipped.left - best->rcWork.left) / width);
        box.relY = static_cast<float>((clipped.top - best->rcWork.top) / height);
        box.relWidth = static_cast<float>((clipped.right - clipped.left) / width);
        box.relHeight = static_cast<float>((clipped.bottom - clipped.top) / height);
        box.assignedApp = !window.processPath.empty() ? window.processPath :
                          !window.aumid.empty() ? window.aumid : window.processName;
        box.exeName = window.processName;
        box.titleHint = window.title;
        box.aumid = window.aumid;
        boxes.push_back(std::move(box));
    }
    return boxes;
}
