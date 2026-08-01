#pragma once
#include <windows.h>
#include <string>
#include <functional>
#include <unordered_map>

class HotkeyManager {
public:
    // Registers a global Windows shortcut (e.g., "CTRL+ALT+C") linked to an ID
    static bool RegisterGlobalHotkey(int id, UINT modifiers, UINT vkKey);

    // Unregisters a specific hotkey by ID
    static void UnregisterGlobalHotkey(int id);

    // Parses human-readable strings like "CTRL+ALT+C" into Win32 VK_KEY and MODIFIER flags
    static bool ParseHotkeyString(const std::string& hotkeyStr, UINT& outModifiers, UINT& outVkKey);
};