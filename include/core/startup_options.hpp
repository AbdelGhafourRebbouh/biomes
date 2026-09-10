#pragma once
#include <string>
namespace biomes {
struct StartupOptions {
    bool silent = false;
    static StartupOptions Parse(const std::wstring& commandLine);
};
}
