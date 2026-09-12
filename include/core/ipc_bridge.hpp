#pragma once
#include "external/nlohmann/json.hpp"
#include <functional>
#include <string>

namespace biomes {
// Transport-independent routing. All callbacks run on the native owner thread,
// after WebViewWindow has validated the page and left the COM callback.
class IpcBridge {
public:
    using Json = nlohmann::json;
    struct Services {
        std::function<Json()> state, topology, settings;
        std::function<Json(const Json&)> updateSettings, saveLayout, snap;
        std::function<Json(const std::string&)> loadLayout;
    };
    IpcBridge(Services services, std::function<void(const std::string&)> send);
    // False means a valid command belongs to the legacy dashboard router.
    bool Handle(const std::string& message);
    void PublishState();
    void Broadcast(const std::string& event, Json fields = Json::object());
    static Json SnapRatios(const Json& request);
private:
    Services services_;
    std::function<void(const std::string&)> send_;
};
}
