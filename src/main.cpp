#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <iostream>
#include <string>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <unordered_set>
#include <unordered_map>
#include <algorithm>
#include <cctype>
#include "../include/external/nlohmann/json.hpp"

#include "../include/ui/webview_window.hpp"
#include "../include/core/window_scaler.hpp"
#include "../include/ui/grid_overlay.hpp"
#include "../include/core/monitor_manager.hpp"
#include "../include/core/json_manager.hpp"
#include "../include/core/biome_manager.hpp"
#include "../include/core/hotkey_manager.hpp"

using json = nlohmann::json;

// =============================================================================
// Biomes IPC contract (JS ⇄ C++ via WebView2 postMessage JSON objects)
//
// UI → native
//   CREATE_NEW_BIOME / SHOW_DESKTOP  open grid overlay after clean slate
//   GET_ACTIVE_WINDOWS               list taskbar-eligible windows
//   GET_SAVED_BIOMES                 load dashboard cards
//   SAVE_BIOME {name,hotkey,coverImagePath,boxes[]}
//   DELETE_BIOME {id}
//   ACTIVATE_BIOME {id}              toggle launch/close
//   CLOSE_BIOME / RESTORE_ALL        close active biome + restore placements
//
// Native → UI
//   LOADED_BIOMES {biomes,activeId}
//   GRID_LAYOUT_READY {boxes}
//   ACTIVE_WINDOWS_LIST {windows}
//   BIOME_SAVED
//   ACTIVE_BIOME_CHANGED {id}
//   STATUS {payload,success?}
// =============================================================================

namespace {
std::string g_activeBiomeId;
bool g_activationInProgress = false;

// Per-zone sticky HWND for the currently active biome (boxId → hwnd).
// Cleared when the biome closes. Prevents stealing sibling Chrome windows
// after the user closes the window that was originally snapped.
std::unordered_map<int, HWND> g_stickyHwnds;

// Minimum score to reuse an existing window without launching.
// Exact sticky = 1000, path = 100, title = 50, exe-only = 10.
constexpr int kMinReuseScore = 50;
constexpr int kExeOnlyScore = 10;
}

void WriteRuntimeLog(const std::string& message) {
    CreateDirectoryA("config", nullptr);
    std::ofstream log("config/biomes_runtime.log", std::ios::app);
    if (log.is_open()) {
        log << message << std::endl;
    }
}

std::string BuildFileUrl(const std::filesystem::path& path) {
    std::string raw = path.string();
    std::replace(raw.begin(), raw.end(), '\\', '/');
    return "file:///" + raw;
}

std::string EscapeJsonString(const std::string& input) {
    std::ostringstream ss;
    for (char c : input) {
        switch (c) {
            case '\\': ss << "\\\\"; break;
            case '"':  ss << "\\\""; break;
            case '\b': ss << "\\b";  break;
            case '\f': ss << "\\f";  break;
            case '\n': ss << "\\n";  break;
            case '\r': ss << "\\r";  break;
            case '\t': ss << "\\t";  break;
            default:
                if ('\x00' <= c && c <= '\x1f') {
                    ss << "\\u" << std::hex << (int)c;
                } else {
                    ss << c;
                }
        }
    }
    return ss.str();
}

std::filesystem::path GetBiomesConfigPath();
json SerializeBox(const SelectedBox& box);
SelectedBox DeserializeBox(const json& value);
void SendSavedBiomesToUi();
void SyncHotkeysFromDisk();
bool ActivateBiome(const std::string& biomeId, std::string& status);
bool DeactivateActiveBiome(std::string& status);
bool ToggleBiome(const std::string& biomeId, std::string& status);

std::string ExecutableName(const std::string& path) {
    return std::filesystem::path(path).filename().string();
}

std::string ToLowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string ExpectedExe(const SelectedBox& box) {
    if (!box.exeName.empty()) return box.exeName;
    return ExecutableName(box.assignedApp);
}

bool SameExe(const WindowInfo& window, const SelectedBox& box) {
    const std::string expected = ExpectedExe(box);
    return !expected.empty() && _stricmp(window.processName.c_str(), expected.c_str()) == 0;
}

int TitleSimilarityScore(const std::string& windowTitle, const std::string& titleHint) {
    if (titleHint.empty() || windowTitle.empty()) return 0;
    const std::string a = ToLowerCopy(windowTitle);
    const std::string b = ToLowerCopy(titleHint);
    if (a == b) return 80;
    if (a.find(b) != std::string::npos || b.find(a) != std::string::npos) return 50;

    // Token overlap (split on spaces).
    auto tokens = [](const std::string& s) {
        std::vector<std::string> out;
        std::string cur;
        for (char ch : s) {
            if (ch == ' ' || ch == '\t' || ch == '-' || ch == '|') {
                if (cur.size() >= 3) out.push_back(cur);
                cur.clear();
            } else {
                cur.push_back(ch);
            }
        }
        if (cur.size() >= 3) out.push_back(cur);
        return out;
    };

    const auto ta = tokens(a);
    const auto tb = tokens(b);
    if (ta.empty() || tb.empty()) return 0;

    int hits = 0;
    for (const auto& x : ta) {
        for (const auto& y : tb) {
            if (x == y) ++hits;
        }
    }
    if (hits >= 2) return 40;
    if (hits == 1) return 20;
    return 0;
}

// Score a candidate for a zone. Higher is better. 0 = not a candidate.
int ScoreWindowForBox(const WindowInfo& window,
                      const SelectedBox& box,
                      HWND stickyHwnd) {
    if (!SameExe(window, box)) return 0;

    int score = kExeOnlyScore;

    if (stickyHwnd && window.hwnd == stickyHwnd) {
        score += 1000;
    }

    if (!box.assignedApp.empty() && box.assignedApp.find('\\') != std::string::npos &&
        !window.processPath.empty() &&
        _stricmp(window.processPath.c_str(), box.assignedApp.c_str()) == 0) {
        score += 100;
    }

    score += TitleSimilarityScore(window.title, box.titleHint);
    return score;
}

struct MatchResult {
    HWND hwnd = nullptr;
    int score = 0;
    int sameExeCount = 0;
    bool stickyDead = false; // had sticky HWND but it no longer exists
};

MatchResult FindBestWindowForBox(const SelectedBox& box,
                                 const std::vector<WindowInfo>& windows,
                                 const std::unordered_set<HWND>& usedWindows) {
    MatchResult result;

    HWND sticky = nullptr;
    const auto stickyIt = g_stickyHwnds.find(box.id);
    if (stickyIt != g_stickyHwnds.end()) {
        if (!stickyIt->second || !IsWindow(stickyIt->second)) {
            result.stickyDead = true;
            g_stickyHwnds.erase(stickyIt);
        } else if (!usedWindows.count(stickyIt->second)) {
            sticky = stickyIt->second;
        }
    }

    for (const auto& candidate : windows) {
        if (usedWindows.count(candidate.hwnd)) continue;
        if (!SameExe(candidate, box)) continue;
        ++result.sameExeCount;

        const int score = ScoreWindowForBox(candidate, box, sticky);
        if (score > result.score) {
            result.score = score;
            result.hwnd = candidate.hwnd;
        }
    }

    return result;
}

bool ShouldReuseExisting(const MatchResult& match) {
    if (!match.hwnd || match.score <= 0) return false;

    // Sticky or path/title-strong match: always reuse.
    if (match.score >= kMinReuseScore) return true;

    // Exe-only match: reuse only when this is the unique unused window.
    // Multiple Chrome windows + weak score → launch new instead of stealing sibling.
    if (match.score <= kExeOnlyScore && match.sameExeCount > 1) return false;

    // Sticky was closed: never fall back to a weak sibling steal.
    if (match.stickyDead && match.score <= kExeOnlyScore) return false;

    return match.sameExeCount == 1;
}

int ResolveMonitorIndex(const SelectedBox& box) {
    if (box.monitorDevice.empty()) return box.monitorIndex;

    struct Ctx {
        std::string device;
        int found = -1;
        int index = 0;
    } ctx{ box.monitorDevice, -1, 0 };

    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR hMonitor, HDC, LPRECT, LPARAM lParam) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(lParam);
        MONITORINFOEXA info = {};
        info.cbSize = sizeof(info);
        if (GetMonitorInfoA(hMonitor, &info) && c->device == info.szDevice) {
            c->found = c->index;
            return FALSE;
        }
        ++c->index;
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));

    return ctx.found >= 0 ? ctx.found : box.monitorIndex;
}

void ClearStickyHwnds() {
    g_stickyHwnds.clear();
}

void NotifyActiveBiomeChanged() {
    WebViewWindow::SendMessageToUI(
        "{\"action\":\"ACTIVE_BIOME_CHANGED\",\"id\":\"" + EscapeJsonString(g_activeBiomeId) + "\"}"
    );
}

bool DeactivateActiveBiome(std::string& status) {
    if (g_activeBiomeId.empty()) {
        status = "No Biome is currently open.";
        return false;
    }

    WindowScaler::RestoreAllCapturedWindows();
    // Keep g_stickyHwnds so the next open can reuse the same HWNDs.
    // Dead HWNDs (user closed Chrome) are detected on next ActivateBiome.
    g_activeBiomeId.clear();
    status = "Biome closed. Windows restored to their previous positions.";
    NotifyActiveBiomeChanged();
    return true;
}

bool ActivateBiome(const std::string& biomeId, std::string& status) {
    if (g_activationInProgress) {
        status = "A Biome launch is already in progress.";
        return false;
    }
    g_activationInProgress = true;

    std::vector<BiomeProfile> profiles;
    if (!JsonManager::LoadBiomesFromFile(GetBiomesConfigPath().string(), profiles)) {
        g_activationInProgress = false;
        status = "Could not read saved Biomes.";
        return false;
    }

    const auto profile = std::find_if(profiles.begin(), profiles.end(), [&](const BiomeProfile& item) {
        return item.id == biomeId;
    });
    if (profile == profiles.end()) {
        g_activationInProgress = false;
        status = "The selected Biome no longer exists.";
        return false;
    }

    // Switching to a different biome: restore + drop sticky map for the old layout.
    if (!g_activeBiomeId.empty() && g_activeBiomeId != biomeId) {
        WindowScaler::RestoreAllCapturedWindows();
        ClearStickyHwnds();
        g_activeBiomeId.clear();
    } else if (!g_activeBiomeId.empty() && g_activeBiomeId == biomeId) {
        // Already active — ToggleBiome should have closed; treat as refresh.
        WindowScaler::RestoreAllCapturedWindows();
        g_activeBiomeId.clear();
    }

    std::vector<WindowInfo> activeWindows = WindowScaler::GetActiveWindows();
    std::unordered_set<HWND> keepVisible;
    std::unordered_set<HWND> tentativelyUsed;

    // Only keep windows we would actually reuse (strong match), not every chrome.exe.
    for (const auto& box : profile->layout) {
        if (box.assignedApp.empty()) continue;
        const MatchResult match = FindBestWindowForBox(box, activeWindows, tentativelyUsed);
        if (ShouldReuseExisting(match) && match.hwnd) {
            keepVisible.insert(match.hwnd);
            tentativelyUsed.insert(match.hwnd);
        }
    }

    WindowScaler::PrepareCleanSlate(WebViewWindow::GetHwnd(), keepVisible);
    Sleep(150);

    activeWindows = WindowScaler::GetActiveWindows();
    std::unordered_set<HWND> usedWindows;
    size_t placed = 0;
    size_t launched = 0;
    size_t failed = 0;
    size_t skippedUwp = 0;
    std::vector<std::string> zoneNotes;

    for (auto box : profile->layout) {
        if (box.assignedApp.empty()) continue;
        box.monitorIndex = ResolveMonitorIndex(box);

        const std::string label = !ExpectedExe(box).empty() ? ExpectedExe(box) : box.assignedApp;

        if (WindowScaler::IsUnsupportedUwpBinding(box.assignedApp)) {
            ++skippedUwp;
            ++failed;
            zoneNotes.push_back(label + ": skipped (UWP/ApplicationFrameHost)");
            continue;
        }

        const MatchResult match = FindBestWindowForBox(box, activeWindows, usedWindows);

        if (ShouldReuseExisting(match)) {
            if (WindowScaler::SnapToBox(match.hwnd, box)) {
                usedWindows.insert(match.hwnd);
                g_stickyHwnds[box.id] = match.hwnd;
                ++placed;
                zoneNotes.push_back(label + ": placed existing (score " + std::to_string(match.score) + ")");
            } else {
                ++failed;
                zoneNotes.push_back(label + ": snap failed");
            }
            continue;
        }

        // Sticky dead or ambiguous multi-instance → launch a NEW window.
        // Exclude every currently known HWND so we never steal a sibling Chrome.
        std::unordered_set<HWND> exclude = usedWindows;
        for (const auto& win : activeWindows) {
            exclude.insert(win.hwnd);
        }

        HWND newHwnd = WindowScaler::LaunchAndSnapApp(box.assignedApp, box, exclude);
        if (newHwnd) {
            usedWindows.insert(newHwnd);
            g_stickyHwnds[box.id] = newHwnd;
            ++placed;
            ++launched;
            if (match.stickyDead) {
                zoneNotes.push_back(label + ": launched new (previous window closed)");
            } else if (match.sameExeCount > 1) {
                zoneNotes.push_back(label + ": launched new (avoided sibling window)");
            } else {
                zoneNotes.push_back(label + ": launched");
            }
            activeWindows = WindowScaler::GetActiveWindows();
        } else {
            ++failed;
            zoneNotes.push_back(label + ": failed to launch/place");
        }
    }

    g_activeBiomeId = biomeId;
    NotifyActiveBiomeChanged();
    g_activationInProgress = false;

    std::ostringstream summary;
    summary << "Biome launched: " << placed << " placed";
    if (launched > 0) summary << " (" << launched << " newly launched)";
    if (failed > 0) summary << ", " << failed << " failed";
    if (skippedUwp > 0) summary << " [UWP slots need a real .exe]";
    if (!zoneNotes.empty()) {
        summary << " | ";
        for (size_t i = 0; i < zoneNotes.size(); ++i) {
            if (i) summary << "; ";
            summary << zoneNotes[i];
        }
    }

    status = summary.str();
    WriteRuntimeLog("[APP] " + status);
    for (const auto& note : zoneNotes) {
        std::cout << "[PLACE] " << note << std::endl;
    }
    return failed == 0;
}

bool ToggleBiome(const std::string& biomeId, std::string& status) {
    if (!biomeId.empty() && g_activeBiomeId == biomeId) {
        return DeactivateActiveBiome(status);
    }
    return ActivateBiome(biomeId, status);
}

void SyncHotkeysFromDisk() {
    std::vector<BiomeProfile> profiles;
    JsonManager::LoadBiomesFromFile(GetBiomesConfigPath().string(), profiles);
    HotkeyManager::SyncBiomeHotkeys(WebViewWindow::GetHwnd(), profiles);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    // Per-monitor DPI so overlay client coords and SetWindowPos stay aligned.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    AllocConsole();
    freopen_s((FILE**)stdout, "CONOUT$", "w", stdout);
    freopen_s((FILE**)stderr, "CONOUT$", "w", stderr);

    std::cout << "=== Biomes Workspace Engine Active ===" << std::endl;
    WriteRuntimeLog("[APP] Startup begin");

    GridOverlay::SetCompletedCallback([](const std::vector<SelectedBox>& boxes) {
        WebViewWindow::RestoreDashboard();
        json payload;
        payload["action"] = "GRID_LAYOUT_READY";
        payload["boxes"] = json::array();
        for (const auto& box : boxes) payload["boxes"].push_back(SerializeBox(box));
        WebViewWindow::SendMessageToUI(payload.dump());
    });
    GridOverlay::SetCancelledCallback([]() {
        WebViewWindow::RestoreDashboard();
        WebViewWindow::SendMessageToUI(R"({"action":"STATUS","payload":"Layout creation cancelled."})");
    });

    WebViewWindow::SetHotkeyPressedCallback([](int hotkeyId) {
        const std::string biomeId = HotkeyManager::ResolveBiomeId(hotkeyId);
        if (biomeId.empty()) return;
        std::string status;
        ToggleBiome(biomeId, status);
        WebViewWindow::SendMessageToUI(
            "{\"action\":\"STATUS\",\"payload\":\"" + EscapeJsonString(status) + "\"}"
        );
    });

    WebViewWindow::SetMessageReceivedCallback([](const std::string& message) {
        try {
            std::cout << "[IPC RECEIVED RAW]: " << message << std::endl;
            WriteRuntimeLog("[APP] IPC raw message: " + message);

            json request = json::parse(message);
            if (!request.is_object()) {
                WebViewWindow::SendMessageToUI(R"({"action":"STATUS","payload":"Invalid request format."})");
                return;
            }

            const std::string action = request.value("action", "");
            WriteRuntimeLog("[APP] Action extracted: " + action);

            if (action == "CREATE_NEW_BIOME" || action == "SHOW_DESKTOP") {
                WriteRuntimeLog("[APP] CREATE_NEW_BIOME — hide dashboard, fullscreen overlay");
                WindowScaler::PrepareCleanSlate(WebViewWindow::GetHwnd(), {});
                WebViewWindow::HideDashboard();
                Sleep(150);

                if (!GridOverlay::ShowOverlay()) {
                    WriteRuntimeLog("[APP] Grid overlay could not be opened");
                    WebViewWindow::RestoreDashboard();
                    WebViewWindow::SendMessageToUI(R"({"action":"STATUS","payload":"Could not open the grid overlay."})");
                }
            }
            else if (action == "GET_ACTIVE_WINDOWS") {
                auto windows = WindowScaler::GetActiveWindows();

                std::ostringstream jsonOut;
                jsonOut << "{\"action\":\"ACTIVE_WINDOWS_LIST\", \"count\":" << windows.size() << ", \"windows\":[";
                for (size_t i = 0; i < windows.size(); ++i) {
                    const std::string appPath = windows[i].processPath.empty()
                        ? windows[i].processName
                        : windows[i].processPath;
                    jsonOut << "{"
                         << "\"hwnd\":" << (uintptr_t)windows[i].hwnd << ","
                         << "\"title\":\"" << EscapeJsonString(windows[i].title) << "\","
                         << "\"process\":\"" << EscapeJsonString(windows[i].processName) << "\","
                         << "\"path\":\"" << EscapeJsonString(appPath) << "\""
                         << "}";
                    if (i + 1 < windows.size()) jsonOut << ",";
                }
                jsonOut << "]}";
                WebViewWindow::SendMessageToUI(jsonOut.str());
            }
            else if (action == "RESTORE_ALL" || action == "CLOSE_BIOME") {
                std::string status;
                DeactivateActiveBiome(status);
                WebViewWindow::SendMessageToUI(
                    "{\"action\":\"STATUS\",\"payload\":\"" + EscapeJsonString(status) + "\"}"
                );
            }
            else if (action == "GET_SAVED_BIOMES") {
                SendSavedBiomesToUi();
            }
            else if (action == "SAVE_BIOME") {
                const std::string name = request.value("name", "");
                if (name.empty() || !request.contains("boxes") || !request["boxes"].is_array()) {
                    WebViewWindow::SendMessageToUI(R"({"action":"STATUS","payload":"A Biome name and layout boxes are required."})");
                    return;
                }

                BiomeProfile profile;
                profile.id = request.value("id", "");
                if (profile.id.empty()) profile.id = "biome-" + std::to_string(GetTickCount64());
                profile.name = name;
                profile.hotkey = request.value("hotkey", "");
                profile.coverImagePath = request.value("coverImagePath", "");
                for (const auto& value : request["boxes"]) {
                    profile.layout.push_back(DeserializeBox(value));
                }

                std::vector<BiomeProfile> profiles;
                const auto configPath = GetBiomesConfigPath();
                if (!JsonManager::LoadBiomesFromFile(configPath.string(), profiles)) {
                    WebViewWindow::SendMessageToUI(R"({"action":"STATUS","payload":"Could not read saved Biomes."})");
                    return;
                }

                bool replaced = false;
                for (auto& existing : profiles) {
                    if (existing.id == profile.id) {
                        existing = profile;
                        replaced = true;
                        break;
                    }
                }
                if (!replaced) profiles.push_back(profile);

                std::filesystem::create_directories(configPath.parent_path());
                if (!JsonManager::SaveBiomesToFile(configPath.string(), profiles)) {
                    WebViewWindow::SendMessageToUI(R"({"action":"STATUS","payload":"Could not save this Biome."})");
                    return;
                }

                SyncHotkeysFromDisk();
                SendSavedBiomesToUi();
                WebViewWindow::SendMessageToUI(R"({"action":"BIOME_SAVED"})");
            }
            else if (action == "DELETE_BIOME") {
                const std::string biomeId = request.value("id", "");
                if (biomeId.empty()) {
                    WebViewWindow::SendMessageToUI(R"({"action":"STATUS","payload":"Missing Biome id."})");
                    return;
                }

                std::vector<BiomeProfile> profiles;
                const auto configPath = GetBiomesConfigPath();
                if (!JsonManager::LoadBiomesFromFile(configPath.string(), profiles)) {
                    WebViewWindow::SendMessageToUI(R"({"action":"STATUS","payload":"Could not read saved Biomes."})");
                    return;
                }

                const auto before = profiles.size();
                profiles.erase(
                    std::remove_if(profiles.begin(), profiles.end(),
                        [&](const BiomeProfile& item) { return item.id == biomeId; }),
                    profiles.end()
                );

                if (profiles.size() == before) {
                    WebViewWindow::SendMessageToUI(R"({"action":"STATUS","payload":"Biome not found."})");
                    return;
                }

                if (g_activeBiomeId == biomeId) {
                    std::string ignored;
                    DeactivateActiveBiome(ignored);
                    ClearStickyHwnds();
                }

                if (!JsonManager::SaveBiomesToFile(configPath.string(), profiles)) {
                    WebViewWindow::SendMessageToUI(R"({"action":"STATUS","payload":"Could not delete this Biome."})");
                    return;
                }

                SyncHotkeysFromDisk();
                SendSavedBiomesToUi();
                WebViewWindow::SendMessageToUI(R"({"action":"STATUS","payload":"Biome deleted."})");
            }
            else if (action == "ACTIVATE_BIOME") {
                const std::string biomeId = request.value("id", "");
                std::string status;
                const bool success = ToggleBiome(biomeId, status);
                WebViewWindow::SendMessageToUI(
                    "{\"action\":\"STATUS\",\"payload\":\"" + EscapeJsonString(status) +
                    "\",\"success\":" + (success ? "true" : "false") + "}"
                );
            }
        } catch (const std::exception& e) {
            WriteRuntimeLog("[APP] Exception in message callback: " + std::string(e.what()));
            std::cerr << "[ERROR] Exception: " << e.what() << std::endl;
        } catch (...) {
            WriteRuntimeLog("[APP] Unknown exception in message callback");
            std::cerr << "[ERROR] Unknown exception" << std::endl;
        }
    });

    char buffer[MAX_PATH];
    GetModuleFileNameA(NULL, buffer, MAX_PATH);
    std::filesystem::path exePath(buffer);
    std::filesystem::path htmlPath = exePath.parent_path() / "index.html";
    std::string startUrl = BuildFileUrl(htmlPath);
    WriteRuntimeLog("[APP] Loading dashboard from " + startUrl);

    WriteRuntimeLog("[APP] Initializing WebView2 window");
    if (!WebViewWindow::Initialize(hInstance, nCmdShow, startUrl)) {
        WriteRuntimeLog("[APP] WebView2 initialization failed");
        std::cerr << "[ERROR] Failed to initialize WebView2 window." << std::endl;
        return -1;
    }

    SyncHotkeysFromDisk();
    WebViewWindow::RunMessageLoop();

    HotkeyManager::Clear(WebViewWindow::GetHwnd());
    return 0;
}

std::filesystem::path GetBiomesConfigPath() {
    char buffer[MAX_PATH];
    GetModuleFileNameA(NULL, buffer, MAX_PATH);
    return std::filesystem::path(buffer).parent_path() / "config" / "biomes.json";
}

json SerializeBox(const SelectedBox& box) {
    return {
        {"id", box.id},
        {"monitorIndex", box.monitorIndex},
        {"startCol", box.startCol}, {"endCol", box.endCol},
        {"startRow", box.startRow}, {"endRow", box.endRow},
        {"relX", box.relX}, {"relY", box.relY},
        {"relWidth", box.relWidth}, {"relHeight", box.relHeight},
        {"assignedApp", box.assignedApp},
        {"exeName", box.exeName},
        {"titleHint", box.titleHint},
        {"monitorDevice", box.monitorDevice},
        {"aumid", box.aumid}
    };
}

SelectedBox DeserializeBox(const json& value) {
    SelectedBox box{};
    box.id = value.value("id", 0);
    box.monitorIndex = value.value("monitorIndex", 0);
    box.startCol = value.value("startCol", 0);
    box.endCol = value.value("endCol", 0);
    box.startRow = value.value("startRow", 0);
    box.endRow = value.value("endRow", 0);
    box.relX = value.value("relX", 0.0f);
    box.relY = value.value("relY", 0.0f);
    box.relWidth = value.value("relWidth", 0.0f);
    box.relHeight = value.value("relHeight", 0.0f);
    box.assignedApp = value.value("assignedApp", "");
    box.exeName = value.value("exeName", "");
    box.titleHint = value.value("titleHint", "");
    box.monitorDevice = value.value("monitorDevice", "");
    box.aumid = value.value("aumid", "");
    if (box.exeName.empty() && !box.assignedApp.empty()) {
        box.exeName = ExecutableName(box.assignedApp);
    }
    return box;
}

void SendSavedBiomesToUi() {
    const std::string biomes = JsonManager::LoadBiomesAsJsonString(GetBiomesConfigPath().string());
    WebViewWindow::SendMessageToUI(
        "{\"action\":\"LOADED_BIOMES\",\"biomes\":" + biomes +
        ",\"activeId\":\"" + EscapeJsonString(g_activeBiomeId) + "\"}"
    );
}
