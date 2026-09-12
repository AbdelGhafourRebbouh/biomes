#include "core/ipc_bridge.hpp"
#include <cmath>
#include <stdexcept>
#include <utility>
namespace biomes {
IpcBridge::IpcBridge(Services services, std::function<void(const std::string&)> send)
    : services_(std::move(services)), send_(std::move(send)) {}

IpcBridge::Json IpcBridge::SnapRatios(const Json& request) {
    Json box = request;
    if (request.contains("quadrant")) {
        const auto quadrant = request.at("quadrant").get<std::string>();
        if (quadrant != "top-left" && quadrant != "top-right" &&
            quadrant != "bottom-left" && quadrant != "bottom-right")
            throw std::runtime_error("Unknown quadrant");
        box["relX"] = quadrant.find("right") != std::string::npos ? .5 : 0.;
        box["relY"] = quadrant.find("bottom") != std::string::npos ? .5 : 0.;
        box["relWidth"] = .5; box["relHeight"] = .5;
    }
    const double x = box.at("relX").get<double>(), y = box.at("relY").get<double>();
    const double w = box.at("relWidth").get<double>(), h = box.at("relHeight").get<double>();
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(w) || !std::isfinite(h) ||
        x < 0 || y < 0 || x >= 1 || y >= 1 || w <= 0 || h <= 0 || x+w > 1.0001 || y+h > 1.0001)
        throw std::runtime_error("Invalid grid ratios");
    return box;
}

bool IpcBridge::Handle(const std::string& message) {
    Json response = {{"action", "IPC_ERROR"}, {"success", false}};
    try {
        if (message.size() > 16 * 1024 * 1024) throw std::runtime_error("IPC message too large");
        const auto request = Json::parse(message);
        if (!request.is_object()) throw std::runtime_error("Expected a JSON object");
        const auto action = request.at("action").get<std::string>();
        if (request.contains("requestId")) {
            const auto id = request.at("requestId").get<std::string>();
            if (id.size() > 128) throw std::runtime_error("Request ID too long");
            response["requestId"] = id;
        }
        if (action != "GET_NATIVE_STATE" && action != "GET_MONITOR_TOPOLOGY" &&
            action != "GET_SETTINGS" && action != "UPDATE_SETTINGS" && action != "TOGGLE_AUTOSTART" &&
            action != "SAVE_LAYOUT" && action != "LOAD_LAYOUT" && action != "SNAP_ACTIVE_WINDOW") return false;
        response["action"] = action == "GET_SETTINGS" || action == "UPDATE_SETTINGS" ? "SETTINGS_RESULT" : action + "_RESULT";
        if (action == "GET_NATIVE_STATE") response["state"] = services_.state();
        else if (action == "GET_MONITOR_TOPOLOGY") response["topology"] = services_.topology();
        else if (action == "GET_SETTINGS") response["settings"] = services_.settings();
        else if (action == "UPDATE_SETTINGS" || action == "TOGGLE_AUTOSTART") {
            Json patch;
            if (action == "UPDATE_SETTINGS") patch = request.at("settings");
            else {
                const bool enabled = request.contains("enabled") ? request.at("enabled").get<bool>() :
                    !services_.settings().at("launchAtStartup").get<bool>();
                patch = {{"launchAtStartup", enabled}};
            }
            response["settings"] = services_.updateSettings(patch);
        } else if (action == "SAVE_LAYOUT") {
            if (!request.contains("name") || request.at("name").get<std::string>().empty() ||
                !request.contains("boxes") || !request.at("boxes").is_array())
                throw std::runtime_error("A name and layout boxes are required");
            response["layout"] = services_.saveLayout(request);
        } else if (action == "LOAD_LAYOUT") {
            const auto id = request.at("id").get<std::string>();
            if (id.empty()) throw std::runtime_error("Missing layout id");
            response["layout"] = services_.loadLayout(id);
        } else response["placement"] = services_.snap(SnapRatios(request));
        response["success"] = true;
    } catch (const std::exception& error) { response["error"] = error.what(); }
    send_(response.dump());
    return true;
}
void IpcBridge::PublishState() {
    try { Broadcast("NATIVE_STATE", {{"state", services_.state()}, {"success", true}}); }
    catch (const std::exception& error) { Broadcast("NATIVE_STATE", {{"success", false}, {"error", error.what()}}); }
}
void IpcBridge::Broadcast(const std::string& event, Json fields) {
    fields["action"] = event;
    send_(fields.dump());
}
}
