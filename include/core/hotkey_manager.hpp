#pragma once
#include <windows.h>
#include <string>
#include <functional>
#include <unordered_map>
#include <vector>

struct BiomeProfile;

class HotkeyManager {
public:
    // Registers a global Windows shortcut against the dashboard HWND.
    static bool RegisterGlobalHotkey(HWND hwnd, int id, UINT modifiers, UINT vkKey);

    // Unregisters a specific hotkey by ID.
    static void UnregisterGlobalHotkey(HWND hwnd, int id);

    // Parses human-readable strings like "CTRL+ALT+C" into Win32 modifiers + VK.
    static bool ParseHotkeyString(const std::string& hotkeyStr, UINT& outModifiers, UINT& outVkKey);

    // Clears every registered Biome hotkey, then registers the current profiles.
    static void SyncBiomeHotkeys(HWND hwnd, const std::vector<BiomeProfile>& profiles);

    // Resolves a RegisterHotKey id back to a Biome id (empty if unknown).
    static std::string ResolveBiomeId(int hotkeyId);

    static void Clear(HWND hwnd);

private:
    static std::unordered_map<int, std::string> s_idToBiome;
    static std::unordered_map<std::string, int> s_biomeToId;
    static int s_nextId;
};
