#include "core/native_settings.hpp"
#include "core/app_paths.hpp"
#include <windows.h>
#include <fstream>
#include <system_error>
namespace biomes {
namespace {
using json = nlohmann::json;
struct Lock {
    HANDLE handle;
    Lock() : handle(CreateFileW((AppPaths::Config() / L"settings.lock").c_str(), GENERIC_READ | GENERIC_WRITE, 0,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)) {
        if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("Settings are busy or inaccessible");
    }
    ~Lock() { CloseHandle(handle); }
};
void Save(const json& data) {
    const auto tmp = AppPaths::Config() / L"settings.json.tmp";
    HANDLE file = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot stage settings");
    const auto text = data.dump(2);
    DWORD written = 0;
    const bool okay = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
        written == text.size() && FlushFileBuffers(file);
    CloseHandle(file);
    if (!okay || !MoveFileExW(tmp.c_str(), AppPaths::SettingsFile().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Cannot save settings; previous settings retained");
}
json Load() {
    if (!std::filesystem::exists(AppPaths::SettingsFile()))
        return {{"schemaVersion",1},{"launchAtStartup",false},{"onboardingCompleted",false}};
    if (std::filesystem::file_size(AppPaths::SettingsFile()) > 1024 * 1024) throw std::runtime_error("Settings file too large");
    std::ifstream file(AppPaths::SettingsFile());
    auto data = json::parse(file);
    if (!data.is_object() || data.value("schemaVersion",1) != 1) throw std::runtime_error("Unsupported settings schema");
    for (const auto* key : {"launchAtStartup","onboardingCompleted"}) {
        if (!data.contains(key)) data[key] = false;
        if (!data[key].is_boolean()) throw std::runtime_error("Invalid settings value");
    }
    data["schemaVersion"] = 1;
    return data;
}
}
nlohmann::json NativeSettings::Read() {
    Lock lock;
    auto data = Load();
    if (!std::filesystem::exists(AppPaths::SettingsFile())) Save(data);
    return data;
}
nlohmann::json NativeSettings::Update(const nlohmann::json& patch) {
    if (!patch.is_object()) throw std::runtime_error("Settings patch must be an object");
    for (const auto& item : patch.items()) {
        if ((item.key() != "launchAtStartup" && item.key() != "onboardingCompleted") || !item.value().is_boolean())
            throw std::runtime_error("Unsupported settings field or type");
    }
    Lock lock;
    auto data = Load();
    const auto before = startup_.Read();
    for (const auto& item : patch.items()) data[item.key()] = item.value();
    if (patch.contains("launchAtStartup")) startup_.SetEnabled(data["launchAtStartup"].get<bool>());
    try { Save(data); }
    catch (...) {
        if (patch.contains("launchAtStartup")) startup_.Restore(before);
        throw;
    }
    return data;
}
void NativeSettings::ReconcileStartup() {
    Lock lock;
    auto data = Load();
    data["launchAtStartup"] = startup_.IsEnabled();
    Save(data);
}
}
