#pragma once
#include "external/nlohmann/json.hpp"
#include "core/startup_registration.hpp"
namespace biomes {
class NativeSettings {
public:
    explicit NativeSettings(StartupRegistration& startup) : startup_(startup) {}
    nlohmann::json Read();
    nlohmann::json Update(const nlohmann::json& patch);
    // Reflect an externally removed/changed Run entry; never silently enable it.
    void ReconcileStartup();
private:
    StartupRegistration& startup_;
};
}
