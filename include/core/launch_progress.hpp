#pragma once
#include "json_manager.hpp"
#include "../external/nlohmann/json.hpp"
#include <map>
#include <string>

// Owner-thread state only. Completion means verified placement (including a
// usable window at its app-enforced size), not a launch
// request accepted by Windows. No window operations or callbacks live here.
class LaunchProgressState {
public:
    struct Item { std::string app, state = "opening", detail; };
    unsigned long long session = 0;
    std::string name;
    bool active = false, sealed = false;
    std::map<std::string, Item> items;

    static std::string Key(const SelectedBox& box) {
        return std::to_string(box.id) + ":" + (!box.stableMonitorId.empty() ? box.stableMonitorId
            : !box.monitorDevice.empty() ? box.monitorDevice : std::to_string(box.monitorIndex));
    }
    void Begin(const std::string& biomeName, const std::vector<SelectedBox>& boxes) {
        ++session; name = biomeName; active = true; sealed = false; items.clear();
        for (const auto& box : boxes) if (!box.assignedApp.empty())
            items.emplace(Key(box), Item{box.exeName.empty() ? "App" : box.exeName, "opening", ""});
    }
    bool Update(const SelectedBox& box, const std::string& state, const std::string& detail = "") {
        if (!active) return false;
        auto found = items.find(Key(box));
        if (found == items.end()) return false;
        if (found->second.state == state && found->second.detail == detail) return false;
        found->second.state = state; found->second.detail = detail; return true;
    }
    nlohmann::json Snapshot() const {
        int ready = 0, failed = 0, skipped = 0, waiting = 0;
        auto rows = nlohmann::json::array();
        for (const auto& entry : items) {
            const auto& item = entry.second;
            ready += item.state == "ready" || item.state == "constrained"; failed += item.state == "failed";
            skipped += item.state == "skipped"; waiting += item.state == "waiting";
            rows.push_back({{"app", item.app}, {"state", item.state}, {"detail", item.detail}});
        }
        const int total = static_cast<int>(items.size()) - skipped;
        const bool done = sealed && ready + failed == total;
        const std::string state = !active ? "cancelled" : !done ? "opening"
            : failed || total == 0 ? "partial" : "success";
        return {{"action", "LAUNCH_PROGRESS"}, {"session", session}, {"name", name},
            {"state", state}, {"ready", ready}, {"total", total}, {"failed", failed},
            {"skipped", skipped}, {"waiting", waiting}, {"items", rows}};
    }
};
