#include "../../include/core/hotkey_manager.hpp"
#include "../../include/core/json_manager.hpp"
#include <iostream>
#include <sstream>
#include <algorithm>

using namespace std;

unordered_map<int, string> HotkeyManager::s_idToBiome;
unordered_map<string, int> HotkeyManager::s_biomeToId;
int HotkeyManager::s_nextId = 1;

bool HotkeyManager::RegisterGlobalHotkey(HWND hwnd, int id, UINT modifiers, UINT vkKey) {
    if (!hwnd) return false;
    if (RegisterHotKey(hwnd, id, modifiers | MOD_NOREPEAT, vkKey)) {
        return true;
    }
    cerr << "[HOTKEY] RegisterHotKey failed for id " << id
         << " (Win32 " << GetLastError() << ")" << endl;
    return false;
}

void HotkeyManager::UnregisterGlobalHotkey(HWND hwnd, int id) {
    if (hwnd) UnregisterHotKey(hwnd, id);
}

bool HotkeyManager::ParseHotkeyString(const string& hotkeyStr, UINT& outModifiers, UINT& outVkKey) {
    outModifiers = 0;
    outVkKey = 0;

    if (hotkeyStr.empty()) return false;

    stringstream ss(hotkeyStr);
    string token;

    while (getline(ss, token, '+')) {
        token.erase(0, token.find_first_not_of(" \t"));
        token.erase(token.find_last_not_of(" \t") + 1);
        transform(token.begin(), token.end(), token.begin(), ::toupper);

        if (token == "CTRL" || token == "CONTROL") {
            outModifiers |= MOD_CONTROL;
        } else if (token == "ALT") {
            outModifiers |= MOD_ALT;
        } else if (token == "SHIFT") {
            outModifiers |= MOD_SHIFT;
        } else if (token == "WIN" || token == "SUPER") {
            outModifiers |= MOD_WIN;
        } else if (token.length() == 1 && token[0] >= 'A' && token[0] <= 'Z') {
            outVkKey = token[0];
        } else if (token.length() == 1 && token[0] >= '0' && token[0] <= '9') {
            outVkKey = token[0];
        }
    }

    return (outVkKey != 0 && outModifiers != 0);
}

void HotkeyManager::Clear(HWND hwnd) {
    for (const auto& entry : s_idToBiome) {
        UnregisterGlobalHotkey(hwnd, entry.first);
    }
    s_idToBiome.clear();
    s_biomeToId.clear();
}

void HotkeyManager::SyncBiomeHotkeys(HWND hwnd, const vector<BiomeProfile>& profiles) {
    Clear(hwnd);
    if (!hwnd) return;

    for (const auto& profile : profiles) {
        if (profile.hotkey.empty() || profile.id.empty()) continue;

        UINT modifiers = 0;
        UINT vkKey = 0;
        if (!ParseHotkeyString(profile.hotkey, modifiers, vkKey)) {
            cerr << "[HOTKEY] Skipping invalid hotkey '" << profile.hotkey
                 << "' for biome " << profile.name << endl;
            continue;
        }

        const int id = s_nextId++;
        if (!RegisterGlobalHotkey(hwnd, id, modifiers, vkKey)) {
            continue;
        }

        s_idToBiome[id] = profile.id;
        s_biomeToId[profile.id] = id;
        cout << "[HOTKEY] Registered " << profile.hotkey
             << " -> " << profile.name << endl;
    }
}

string HotkeyManager::ResolveBiomeId(int hotkeyId) {
    const auto it = s_idToBiome.find(hotkeyId);
    return it == s_idToBiome.end() ? string() : it->second;
}
