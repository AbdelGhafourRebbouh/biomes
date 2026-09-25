#include "core/app_updater.hpp"
#include "external/nlohmann/json.hpp"
#include <iostream>
#include <stdexcept>
int main() {
    try {
        const auto require = [](bool ok) { if (!ok) throw std::runtime_error("Updater assertion failed"); };
        nlohmann::json feed = {{"schemaVersion",1},{"channel","beta"},{"build",5},{"version","1.0.0-beta.5"},
            {"url","https://github.com/AbdelGhafourRebbouh/biomes/releases/download/v1.0.0-beta.5/biomesSetup-v1.0.0-beta.5.exe"},
            {"size",3},{"sha256","ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"}};
        const auto release = biomes::AppUpdater::ParseFeed(feed.dump(),4);
        require(release.build == 5);
        require(!biomes::AppUpdater::ParseFeed(feed.dump(),5).build);
        require(!biomes::AppUpdater::ParseFeed(feed.dump(),6).build);
        require(biomes::AppUpdater::VerifyPayload(release, {'a','b','c'}));
        require(!biomes::AppUpdater::VerifyPayload(release, {'a','b','d'}));
        require(!biomes::AppUpdater::VerifyPayload(release, {'a','b'}));
        const auto reject = [&](const nlohmann::json& value) {
            bool rejected = false;
            try { biomes::AppUpdater::ParseFeed(value.dump(),0); } catch (...) { rejected = true; }
            require(rejected);
        };
        for (const auto& field : {"build","size","version","sha256","url","channel","schemaVersion"}) {
            auto invalid = feed; invalid.erase(field); reject(invalid);
        }
        for (const auto build : {-1,0,65536}) { auto invalid=feed; invalid["build"]=build; reject(invalid); }
        for (const auto size : {-1,0,134217729}) { auto invalid=feed; invalid["size"]=size; reject(invalid); }
        for (const auto& url : {"http://github.com/a.exe", "https://example.com/a.exe",
                "https://github.com/another/repo/releases/download/beta/biomesSetup.exe"}) {
            auto invalid=feed; invalid["url"]=url; reject(invalid);
        }
        auto invalid=feed; invalid["version"]="1.0.0-beta.6"; reject(invalid);
        invalid=feed; invalid["sha256"]="not-a-checksum"; reject(invalid);
        invalid=feed; invalid["channel"]="stable"; reject(invalid);
        bool rejected=false; try { biomes::AppUpdater::ParseFeed("{bad",0); } catch (...) { rejected=true; } require(rejected);
        std::cout << "Updater feed validation, monotonic versioning, trusted URLs and SHA-256 tests passed\n";
    } catch(const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
