#pragma once
#include <filesystem>

namespace biomes {
// Run before opening data or WebView2. Throws on incomplete migration;
// original files are retained, and existing destination files always win.
// Only explicitly supplied legacy roots are inspected, never the current directory.
void MigrateLegacyData(const std::filesystem::path& legacyRoot);
}
