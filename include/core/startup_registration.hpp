#pragma once
#include <string>
#include <optional>
#include <filesystem>
namespace biomes {
class StartupRegistration {
public:
    StartupRegistration();
    std::optional<std::wstring> Read() const;
    void Restore(const std::optional<std::wstring>& value) const;
    void SetEnabled(bool enabled) const;
    bool IsEnabled() const;
    static std::wstring Command(const std::filesystem::path& executable);
#ifdef BIOMES_STORAGE_TESTING
    explicit StartupRegistration(std::wstring testKey) : key_(std::move(testKey)) {}
#endif
private:
    std::wstring key_;
};
}
