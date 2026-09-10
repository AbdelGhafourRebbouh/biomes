#include "core/startup_registration.hpp"
#include "core/app_paths.hpp"
#include <windows.h>
#include <system_error>
#include <vector>
namespace biomes {
StartupRegistration::StartupRegistration() : key_(L"Software\\Microsoft\\Windows\\CurrentVersion\\Run") {}
std::wstring StartupRegistration::Command(const std::filesystem::path& executable) {
    if (!executable.is_absolute() || executable.wstring().find(L'"') != std::wstring::npos)
        throw std::runtime_error("Invalid startup executable path");
    return L"\"" + executable.wstring() + L"\" --autostart";
}
std::optional<std::wstring> StartupRegistration::Read() const {
    DWORD size = 0;
    auto result = RegGetValueW(HKEY_CURRENT_USER, key_.c_str(), L"biomes", RRF_RT_REG_SZ, nullptr, nullptr, &size);
    if (result == ERROR_FILE_NOT_FOUND || result == ERROR_PATH_NOT_FOUND) return {};
    if (result != ERROR_SUCCESS) throw std::system_error(result, std::system_category(), "Read startup entry");
    if (size > 65536) throw std::runtime_error("Startup entry too large");
    std::vector<wchar_t> buffer(size / sizeof(wchar_t) + 1);
    result = RegGetValueW(HKEY_CURRENT_USER, key_.c_str(), L"biomes", RRF_RT_REG_SZ, nullptr, buffer.data(), &size);
    if (result != ERROR_SUCCESS) throw std::system_error(result, std::system_category(), "Read startup command");
    return std::wstring(buffer.data());
}
void StartupRegistration::Restore(const std::optional<std::wstring>& value) const {
    if (!value) {
        const auto result = RegDeleteKeyValueW(HKEY_CURRENT_USER, key_.c_str(), L"biomes");
        if (result != ERROR_SUCCESS && result != ERROR_FILE_NOT_FOUND && result != ERROR_PATH_NOT_FOUND)
            throw std::system_error(result, std::system_category(), "Remove startup entry");
        return;
    }
    const auto result = RegSetKeyValueW(HKEY_CURRENT_USER, key_.c_str(), L"biomes", REG_SZ,
        value->c_str(), static_cast<DWORD>((value->size() + 1) * sizeof(wchar_t)));
    if (result != ERROR_SUCCESS) throw std::system_error(result, std::system_category(), "Write startup entry");
}
void StartupRegistration::SetEnabled(bool enabled) const {
    Restore(enabled ? std::make_optional(Command(AppPaths::ExecutableDirectory() / L"Biomes.exe")) : std::nullopt);
}
bool StartupRegistration::IsEnabled() const {
    const auto value = Read();
    return value && *value == Command(AppPaths::ExecutableDirectory() / L"Biomes.exe");
}
}
