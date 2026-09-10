#include "core/startup_options.hpp"
#include <windows.h>
#include <shellapi.h>
#include <stdexcept>
namespace biomes {
StartupOptions StartupOptions::Parse(const std::wstring& commandLine) {
    int count = 0;
    auto args = CommandLineToArgvW(commandLine.c_str(), &count);
    if (!args) throw std::runtime_error("Cannot parse startup options");
    StartupOptions options;
    bool invalid = false;
    for (int i = 1; i < count; ++i) {
        const std::wstring arg(args[i]);
        if (arg == L"--minimized" || arg == L"--autostart") options.silent = true;
        else invalid = true;
    }
    LocalFree(args);
    if (invalid) throw std::runtime_error("Unknown startup option");
    return options;
}
}
