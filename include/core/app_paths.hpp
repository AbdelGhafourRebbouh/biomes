#pragma once
#include <filesystem>

namespace biomes {
class AppPaths final {
public:
    static void Initialize();
    static const std::filesystem::path& Root();
    static std::filesystem::path Config();
    static std::filesystem::path Logs();
    static std::filesystem::path WebViewData();
    static std::filesystem::path Images();
    static std::filesystem::path Backups();
    static std::filesystem::path BiomesFile();
    static std::filesystem::path SettingsFile();
    static std::filesystem::path RuntimeLog();
    static std::filesystem::path ExecutableDirectory();
#ifdef BIOMES_STORAGE_TESTING
    // Compiled out of production. Never redirect real user data in tests.
    static void InitializeForTests(const std::filesystem::path& root);
#endif
private:
    AppPaths() = delete;
};
}
