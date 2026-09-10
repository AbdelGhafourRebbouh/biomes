#include "core/app_paths.hpp"
#include <windows.h>
#include <shlobj.h>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <vector>
#include <string>

namespace biomes {
namespace {
namespace fs = std::filesystem;
std::once_flag initialization;
fs::path dataRoot;
struct CoFree { void operator()(wchar_t* p) const noexcept { CoTaskMemFree(p); } };
void EnsureDirectory(const fs::path& directory) {
    fs::create_directories(directory);
    const DWORD attributes = GetFileAttributesW(directory.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
        throw std::system_error(GetLastError(), std::system_category(), "Inspect data directory");
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
        throw std::runtime_error("Data directory is not a regular directory");
}
void InitializeAt(const fs::path& root) {
    if (!root.is_absolute()) throw std::runtime_error("Data path must be absolute");
    EnsureDirectory(root);
    for (const auto* child : {L"config", L"logs", L"webview_data", L"images", L"backups"})
        EnsureDirectory(root / child);
    dataRoot = root;
}
}
void AppPaths::Initialize() {
    std::call_once(initialization, [] {
        PWSTR raw = nullptr;
        const HRESULT hr = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &raw);
        std::unique_ptr<wchar_t, CoFree> owned(raw);
        if (FAILED(hr)) throw std::system_error(static_cast<int>(hr), std::system_category(), "Resolve LocalAppData");
        if (!raw || !*raw) throw std::runtime_error("LocalAppData is empty");
        InitializeAt((fs::path(raw) / L"biomes").lexically_normal());
    });
}
#ifdef BIOMES_STORAGE_TESTING
void AppPaths::InitializeForTests(const std::filesystem::path& root) {
    std::call_once(initialization, [&] { InitializeAt(root); });
}
#endif
const std::filesystem::path& AppPaths::Root() {
    if (dataRoot.empty()) throw std::logic_error("AppPaths is not initialized");
    return dataRoot;
}
std::filesystem::path AppPaths::Config() { return Root() / L"config"; }
std::filesystem::path AppPaths::Logs() { return Root() / L"logs"; }
std::filesystem::path AppPaths::WebViewData() { return Root() / L"webview_data"; }
std::filesystem::path AppPaths::Images() { return Root() / L"images"; }
std::filesystem::path AppPaths::Backups() { return Root() / L"backups"; }
std::filesystem::path AppPaths::BiomesFile() { return Config() / L"biomes.json"; }
std::filesystem::path AppPaths::SettingsFile() { return Config() / L"settings.json"; }
std::filesystem::path AppPaths::RuntimeLog() { return Logs() / L"biomes_runtime.log"; }
std::filesystem::path AppPaths::ExecutableDirectory() {
    std::vector<wchar_t> buffer(512);
    for (;;) {
        const DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (!size) throw std::system_error(GetLastError(), std::system_category(), "Resolve executable path");
        if (size < buffer.size()) return std::filesystem::path(std::wstring(buffer.data(), size)).parent_path();
        if (buffer.size() >= 32768) throw std::runtime_error("Executable path too long");
        buffer.resize(buffer.size() * 2);
    }
}
}
