#include "core/app_paths.hpp"
#include "core/legacy_data_migration.hpp"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;
void Require(bool condition) { if (!condition) throw std::runtime_error("Storage assertion failed"); }
void Write(const fs::path& p, const char* text) { fs::create_directories(p.parent_path()); std::ofstream(p) << text; }
std::string Read(const fs::path& p) { std::ifstream f(p); return {std::istreambuf_iterator<char>(f), {}}; }
int main(int argc, char** argv) {
    const auto temp = fs::temp_directory_path() / (L"biomes-storage-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    try {
        const std::string scenario = argc > 1 ? argv[1] : "paths";
        const auto root = temp / L"\u7528\u6237-data";
        const auto legacy = temp / L"legacy";
        biomes::AppPaths::InitializeForTests(root);
        Require(biomes::AppPaths::Root() == root);
        for (const auto* name : {L"config", L"logs", L"webview_data", L"images", L"backups"}) Require(fs::is_directory(root / name));
        Require(!fs::exists(biomes::AppPaths::SettingsFile()));
        if (scenario != "paths") {
            Write(legacy / L"config/biomes.json", "{\"version\":3,\"biomes\":[]}");
            Write(legacy / L"config/settings.json", "{\"schemaVersion\":1,\"theme\":\"dark\"}");
            Write(legacy / L"webview_data/Default/Preferences", "legacy-profile");
            Write(legacy / L"config/biomes_runtime.log", "old log");
            if (scenario == "existing") {
                Write(biomes::AppPaths::BiomesFile(), "existing");
                Write(biomes::AppPaths::WebViewData() / L"keep", "existing-profile");
            }
            if (scenario == "invalid") Write(legacy / L"config/biomes.json", "{\"version\":999,\"biomes\":[]}");
            HANDLE locked = INVALID_HANDLE_VALUE;
            if (scenario == "locked") {
                locked = CreateFileW((legacy / L"webview_data/Default/Preferences").c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
                Require(locked != INVALID_HANDLE_VALUE);
            }
            if (scenario == "wrongtype") {
                // A file occupying the legacy profile directory must be rejected.
                fs::remove(legacy / L"webview_data/Default/Preferences");
                fs::remove(legacy / L"webview_data/Default");
                fs::remove(legacy / L"webview_data");
                Write(legacy / L"webview_data", "not a directory");
            }
            bool failed = false;
            try { biomes::MigrateLegacyData(legacy); } catch (...) { failed = true; }
            if (locked != INVALID_HANDLE_VALUE) CloseHandle(locked);
            const bool expectFailure = scenario == "invalid" || scenario == "locked" || scenario == "wrongtype";
            Require(failed == expectFailure);
            Require(fs::exists(legacy / L"config/biomes.json"));
            if (expectFailure) {
                Require(!fs::exists(root / L"backups/legacy-migration-v1.done"));
                Require(!fs::exists(biomes::AppPaths::BiomesFile()));
                if (scenario == "locked") { biomes::MigrateLegacyData(legacy); Require(fs::exists(biomes::AppPaths::BiomesFile())); }
            } else {
                Require(fs::exists(root / L"backups/legacy-migration-v1.done"));
                Require(Read(biomes::AppPaths::BiomesFile()) == (scenario == "existing" ? "existing" : Read(legacy / L"config/biomes.json")));
                Require(fs::exists(biomes::AppPaths::WebViewData() / (scenario == "existing" ? L"keep" : L"Default/Preferences")));
                if (scenario == "once") {
                    Write(legacy / L"config/biomes.json", "broken on purpose");
                    biomes::MigrateLegacyData(legacy);
                    Require(Read(biomes::AppPaths::BiomesFile()) != "broken on purpose");
                }
            }
        }
        // Only delete this test's explicitly owned temporary tree.
        fs::remove_all(temp);
        std::cout << "PASS storage " << scenario << '\n';
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << "; fixtures retained at " << temp << '\n';
        return 1;
    }
}
