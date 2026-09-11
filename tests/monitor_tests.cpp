// Read-only monitor checks; no display settings or user profiles are changed.
#include "../src/core/monitor_manager.cpp"
#include "../include/external/nlohmann/json.hpp"
#include <stdexcept>

int main() {
    try {
        const auto require = [](bool value, const char* message) {
            if (!value) throw std::runtime_error(message);
        };
        require(WideToUtf8(L"Monitor \u00e9") == "Monitor \xc3\xa9", "Unicode conversion");
        require(nlohmann::json::parse("\"" + EscapeJson("name\n\t\x01") + "\"") == "name\n\t\x01", "JSON controls");
        MonitorDetail first{}, second{};
        first.stableId = "EDID:A"; first.deviceName = "DISPLAY1";
        second.stableId = "EDID:B"; second.deviceName = "DISPLAY2";
        require(MonitorManager::GetTopologyHash({first, second}) == MonitorManager::GetTopologyHash({second, first}), "order independent topology");
        require(MonitorManager::GetTopologyHash({first}) != MonitorManager::GetTopologyHash({first, second}), "disconnection changes topology");
        second.stableId = first.stableId;
        std::vector<MonitorDetail> duplicates{first, second};
        DisambiguateDuplicateStableIds(duplicates);
        require(duplicates[0].stableId != duplicates[1].stableId, "duplicate model identities separated");
        first.width = 1920; first.height = 1040; first.dpiX = 144;
        require(MonitorManager::BuildWorkAreaSignature(first) == "1920x1040@150", "snapshot DPI signature");
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        const auto serialized = nlohmann::json::parse(MonitorManager::SerializeMonitorsJson());
        require(serialized.at("monitors").is_array(), "valid monitor JSON");
        for (const auto& monitor : serialized.at("monitors")) {
            require(monitor.at("dpiX").get<UINT>() > 0 && monitor.at("dpiY").get<UINT>() > 0, "valid DPI");
            require(monitor.at("scaleX").get<double>() > 0, "valid scale");
        }
        std::cout << "Monitor regressions passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
