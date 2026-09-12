#include "core/ipc_bridge.hpp"
#include <iostream>
#include <stdexcept>
#include <vector>
int main() {
    using Bridge = biomes::IpcBridge;
    using Json = Bridge::Json;
    try {
        auto require = [](bool value, const char* text) { if (!value) throw std::runtime_error(text); };
        std::vector<Json> output;
        bool enabled = false;
        int saves = 0, loads = 0, snaps = 0;
        Bridge::Services services;
        services.settings = [&] { return Json{{"launchAtStartup", enabled}}; };
        services.updateSettings = [&](const Json& patch) {
            enabled = patch.at("launchAtStartup").get<bool>(); return services.settings();
        };
        services.topology = [] { return Json{{"monitors", Json::array()}, {"topologyHash", "fixture"}}; };
        services.state = [&] { return Json{{"settings", services.settings()}, {"biomes", Json::array()}}; };
        services.saveLayout = [&](const Json&) { ++saves; return Json{{"id", "saved"}}; };
        services.loadLayout = [&](const std::string& id) {
            ++loads; if (id == "missing") throw std::runtime_error("Unknown layout"); return Json{{"id", id}};
        };
        services.snap = [&](const Json& ratios) { ++snaps; return ratios; };
        Bridge bridge(services, [&](const std::string& value) { output.push_back(Json::parse(value)); });
        bridge.PublishState();
        require(output.back().at("action") == "NATIVE_STATE" && output.back().at("success"), "initial state");
        bridge.Handle(R"({"action":"TOGGLE_AUTOSTART","enabled":true,"requestId":"one"})");
        require(enabled && output.back().at("requestId") == "one", "startup routing and correlation");
        bridge.Handle(R"({"action":"TOGGLE_AUTOSTART","enabled":"false"})");
        require(enabled && !output.back().at("success"), "invalid boolean does not mutate");
        bridge.Handle(R"({"action":"GET_SETTINGS"})");
        require(output.back().at("action") == "SETTINGS_RESULT", "legacy settings response");
        bridge.Handle(R"({"action":"UPDATE_SETTINGS","settings":{"launchAtStartup":false}})");
        require(!enabled && output.back().at("success"), "update settings routes");
        bridge.Handle(R"({"action":"GET_MONITOR_TOPOLOGY"})");
        require(output.back().at("topology").at("topologyHash") == "fixture", "topology response");
        bridge.Handle(R"({"action":"SAVE_LAYOUT","name":"Test","boxes":[]})");
        require(saves == 1 && output.back().at("success"), "save routing");
        bridge.Handle(R"({"action":"SAVE_LAYOUT","name":"Test","boxes":5})");
        require(saves == 1 && !output.back().at("success"), "bad layout rejected");
        bridge.Handle(R"({"action":"LOAD_LAYOUT","id":"saved"})");
        require(loads == 1 && output.back().at("layout").at("id") == "saved", "restore routing");
        bridge.Handle(R"({"action":"LOAD_LAYOUT","id":"missing"})");
        require(!output.back().at("success") && output.back().at("error") == "Unknown layout", "service error response");
        bridge.Handle(R"({"action":"SNAP_ACTIVE_WINDOW","quadrant":"bottom-right"})");
        require(snaps == 1 && output.back().at("placement").at("relX") == .5, "quadrant conversion");
        bridge.Handle(R"({"action":"SNAP_ACTIVE_WINDOW","relX":0,"relY":0,"relWidth":0.333333,"relHeight":1})");
        require(snaps == 2 && output.back().at("success"), "custom ratios");
        for (const auto* message : {"[]", "broken", R"({"action":"GET_SETTINGS","requestId":42})",
            R"({"action":"SNAP_ACTIVE_WINDOW","relX":0,"relY":0,"relWidth":2,"relHeight":1})",
            R"({"action":"SNAP_ACTIVE_WINDOW","quadrant":"unknown"})"}) {
            require(bridge.Handle(message) && !output.back().at("success"), "malformed request handled");
        }
        require(snaps == 2, "invalid snaps never reach engine");
        const auto count = output.size();
        require(!bridge.Handle(R"({"action":"CREATE_NEW_BIOME"})") && output.size() == count, "legacy routing preserved");
        bridge.Broadcast("MONITOR_CHANGED", {{"topology", services.topology()}});
        require(output.back().at("action") == "MONITOR_CHANGED", "event broadcast");
        bridge.Handle(Json{{"action", "GET_SETTINGS"}, {"requestId", std::string(129, 'x')}}.dump());
        require(!output.back().at("success"), "oversized request id");
        std::cout << "IPC regressions passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
