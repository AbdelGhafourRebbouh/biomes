#include "../../include/core/hotkey_manager.hpp"
#include <iostream>
#include <sstream>
#include <algorithm>
using namespace std;

bool HotkeyManager::RegisterGlobalHotkey(int id, UINT modifiers, UINT vkKey) {
    // MOD_NOREPEAT prevents firing hundreds of times if the user holds the keys down
    if (RegisterHotKey(NULL, id, modifiers | MOD_NOREPEAT, vkKey)) {
        return true;
    }
    return false;
}

void HotkeyManager::UnregisterGlobalHotkey(int id) {
    UnregisterHotKey(NULL, id);
}

bool HotkeyManager::ParseHotkeyString(const string& hotkeyStr, UINT& outModifiers, UINT& outVkKey) {
    outModifiers = 0;
    outVkKey = 0;

    if (hotkeyStr.empty()) return false;

    stringstream ss(hotkeyStr);
    string token;

    while (getline(ss, token, '+')) {
        // Trim spaces and convert to uppercase
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
            outVkKey = token[0]; // ASCII 'A'-'Z' matches Win32 Virtual Key codes
        } else if (token.length() == 1 && token[0] >= '0' && token[0] <= '9') {
            outVkKey = token[0];
        }
    }

    return (outVkKey != 0);
}