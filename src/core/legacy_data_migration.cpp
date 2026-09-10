#include "core/legacy_data_migration.hpp"
#include "core/app_paths.hpp"
#include "external/nlohmann/json.hpp"
#include <windows.h>
#include <objbase.h>
#include <fstream>
#include <vector>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <string>

namespace biomes {
namespace {
namespace fs = std::filesystem;
using json = nlohmann::json;
struct Close { void operator()(void* h) const noexcept { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); } };
using Handle = std::unique_ptr<void, Close>;
Handle Open(const fs::path& p, DWORD access, DWORD sharing, DWORD creation) {
    HANDLE h = CreateFileW(p.c_str(), access, sharing, nullptr, creation, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) throw std::system_error(GetLastError(), std::system_category(), "Legacy data unavailable; close other biomes instances");
    return Handle(h);
}
bool Exists(const fs::path& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    if (a != INVALID_FILE_ATTRIBUTES) return true;
    DWORD e = GetLastError();
    if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) return false;
    throw std::system_error(e, std::system_category(), "Inspect migration path");
}
void RejectLinks(const fs::path& p) {
    for (auto part = p; !part.empty(); part = part.parent_path()) {
        const DWORD a = GetFileAttributesW(part.c_str());
        if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("Migration path is unavailable or contains a link");
        if (part == part.root_path()) break;
    }
}
void ValidateJson(const fs::path& p, bool biomes) {
    std::ifstream input(p);
    const auto data = json::parse(input);
    if (!data.is_object()) throw std::runtime_error("Legacy configuration is not an object");
    if (biomes) {
        const int version = data.value("version", 1);
        if (version < 1 || version > 3 || !data.contains("biomes") || !data["biomes"].is_array())
            throw std::runtime_error("Unsupported legacy biomes format");
        for (const auto& item : data["biomes"]) {
            if (!item.is_object() || !item.contains("id") || !item["id"].is_string() ||
                !item.contains("name") || !item["name"].is_string())
                throw std::runtime_error("Invalid legacy biome record");
            for (const auto* key : {"hotkey", "coverImagePath", "topologyHash"})
                if (item.contains(key) && !item[key].is_string()) throw std::runtime_error("Invalid legacy biome field");
            auto validateBoxes = [](const json& boxes) {
                if (!boxes.is_array()) throw std::runtime_error("Invalid legacy boxes");
                for (const auto& box : boxes) {
                    if (!box.is_object()) throw std::runtime_error("Invalid legacy box");
                    for (const auto* key : {"id", "monitorIndex", "startCol", "endCol", "startRow", "endRow"})
                        if (box.contains(key) && !box[key].is_number_integer()) throw std::runtime_error("Invalid legacy integer");
                    for (const auto* key : {"relX", "relY", "relWidth", "relHeight"})
                        if (box.contains(key) && !box[key].is_number()) throw std::runtime_error("Invalid legacy geometry");
                    for (const auto* key : {"assignedApp", "exeName", "titleHint", "monitorDevice", "stableMonitorId", "topologyHash", "aumid", "launchUri"})
                        if (box.contains(key) && !box[key].is_string()) throw std::runtime_error("Invalid legacy app field");
                }
            };
            if (item.contains("boxes")) validateBoxes(item["boxes"]);
            if (item.contains("layoutVariants")) {
                if (!item["layoutVariants"].is_object()) throw std::runtime_error("Invalid legacy variants");
                for (const auto& variant : item["layoutVariants"]) validateBoxes(variant);
            }
        }
    } else if (data.value("schemaVersion", 1) != 1 || data.value("version", 1) != 1) {
        throw std::runtime_error("Unsupported legacy settings format");
    }
}
// Keep read locks until all staging and promotion finishes. This refuses files
// currently open for writing, including an active WebView2 profile.
void CopyLocked(const fs::path& from, const fs::path& to, std::vector<Handle>& locks) {
    RejectLinks(from);
    if (fs::is_directory(from)) {
        fs::create_directory(to);
        for (const auto& entry : fs::directory_iterator(from))
            CopyLocked(entry.path(), to / entry.path().filename(), locks);
    } else {
        locks.push_back(Open(from, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING));
        if (!CopyFileW(from.c_str(), to.c_str(), TRUE))
            throw std::system_error(GetLastError(), std::system_category(), "Copy legacy data");
    }
}
struct Item { fs::path source, target, staged; bool directory; };
}
void MigrateLegacyData(const std::filesystem::path& legacyRoot) {
    if (!legacyRoot.is_absolute()) throw std::runtime_error("Legacy root must be absolute");
    if (!Exists(legacyRoot)) return;
    RejectLinks(legacyRoot);
    if (fs::equivalent(legacyRoot, AppPaths::Root())) return;
    const auto marker = AppPaths::Backups() / L"legacy-migration-v1.done";
    // A file lock, rather than a mutex name, scopes migration to this data root.
    auto migrationLock = Open(AppPaths::Backups() / L"migration.lock", GENERIC_READ | GENERIC_WRITE, 0, OPEN_ALWAYS);
    if (Exists(marker)) return;
    std::vector<Item> items;
    auto add = [&](const fs::path& source, const fs::path& target, bool directory) {
        if (!Exists(source)) return;
        RejectLinks(source);
        if (Exists(target)) {
            RejectLinks(target);
            if (!directory || !fs::is_directory(target) || !fs::is_empty(target)) return;
        }
        if (fs::is_directory(source) != directory) throw std::runtime_error("Unexpected legacy file type");
        items.push_back({source, target, {}, directory});
    };
    add(legacyRoot / L"config/biomes.json", AppPaths::BiomesFile(), false);
    add(legacyRoot / L"config/settings.json", AppPaths::SettingsFile(), false);
    add(legacyRoot / L"config/biomes_runtime.log", AppPaths::Logs() / L"legacy-biomes_runtime.log", false);
    add(legacyRoot / L"webview_data", AppPaths::WebViewData(), true);
    // The installed images folder contains shipped assets, not user-owned media.
    // Existing embedded cover data remains byte-for-byte inside biomes.json.
    if (items.empty()) return;
    GUID id{};
    if (FAILED(CoCreateGuid(&id))) throw std::runtime_error("Create migration staging identifier");
    wchar_t name[40]{}; StringFromGUID2(id, name, 40);
    const auto stage = AppPaths::Backups() / (std::wstring(L"legacy-stage-") + name);
    fs::create_directory(stage);
    std::vector<Handle> locks;
    // Stage everything before promoting anything. Failed staging is retained
    // for diagnosis; originals and destinations are not deleted or replaced.
    for (size_t i = 0; i < items.size(); ++i) {
        auto& item = items[i];
        item.staged = stage / std::to_wstring(i);
        CopyLocked(item.source, item.staged, locks);
        if (item.target == AppPaths::BiomesFile()) ValidateJson(item.staged, true);
        if (item.target == AppPaths::SettingsFile()) ValidateJson(item.staged, false);
    }
    for (auto& item : items) {
        if (Exists(item.target)) {
            RejectLinks(item.target);
            if (!item.directory || !fs::is_directory(item.target) || !fs::is_empty(item.target)) continue;
            // Remove only the verified empty placeholder created by AppPaths.
            if (!RemoveDirectoryW(item.target.c_str()))
                throw std::system_error(GetLastError(), std::system_category(), "Prepare empty profile destination");
        }
        if (!MoveFileExW(item.staged.c_str(), item.target.c_str(), MOVEFILE_WRITE_THROUGH))
            throw std::system_error(GetLastError(), std::system_category(), "Promote migration without overwrite");
    }
    const auto stagedMarker = stage / L"complete";
    auto markerFile = Open(stagedMarker, GENERIC_WRITE, 0, CREATE_NEW);
    const std::string note = "Migration v1 completed. Legacy originals retained.\r\nSource: " + legacyRoot.u8string();
    DWORD written = 0;
    if (!WriteFile(markerFile.get(), note.data(), static_cast<DWORD>(note.size()), &written, nullptr) ||
        written != note.size() || !FlushFileBuffers(markerFile.get()))
        throw std::runtime_error("Could not persist migration completion");
    markerFile.reset();
    if (!MoveFileExW(stagedMarker.c_str(), marker.c_str(), MOVEFILE_WRITE_THROUGH))
        throw std::system_error(GetLastError(), std::system_category(), "Record migration completion");
}
}
