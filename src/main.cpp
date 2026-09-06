#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <iostream>
#include <string>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <unordered_set>
#include <unordered_map>
#include <utility>
#include <algorithm>
#include <cctype>
#include "../include/external/nlohmann/json.hpp"

#include "../include/ui/webview_window.hpp"
#include "../include/core/window_scaler.hpp"
#include "../include/ui/grid_overlay.hpp"
#include "../include/core/monitor_manager.hpp"
#include "../include/core/json_manager.hpp"
#include "../include/core/hotkey_manager.hpp"
#include "../include/core/app_launcher.hpp"

using json = nlohmann::json;

// =============================================================================
// Biomes IPC contract (JS ⇄ C++ via WebView2 postMessage JSON objects)
//
// UI → native
//   CREATE_NEW_BIOME / SHOW_DESKTOP  open grid overlay after clean slate
//   GET_ACTIVE_WINDOWS               list taskbar-eligible windows
//   GET_SAVED_BIOMES                 load dashboard cards
//   SAVE_BIOME {name,hotkey,coverImagePath,boxes[]}
//   FIX_BIOME_LAYOUT {id}            reopen overlay with remapped zones
//   DELETE_BIOME {id}
//   ACTIVATE_BIOME {id}              toggle launch/close
//   CLOSE_BIOME / RESTORE_ALL        close active biome + restore placements
//
// Native → UI
//   LOADED_BIOMES {biomes,activeId}
//   MONITORS_CHANGED {payload:{topologyHash,monitors[]}}
//   GRID_LAYOUT_READY {boxes}
//   ACTIVE_WINDOWS_LIST {windows}
//   BIOME_SAVED
//   ACTIVE_BIOME_CHANGED {id}
//   STATUS {payload,success?}
// =============================================================================

namespace {
std::string g_activeBiomeId;
bool g_recordingHotkey = false;
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

bool SameAumid(const WindowInfo& window, const SelectedBox& box) {
    return !box.aumid.empty() && !window.aumid.empty() &&
           _stricmp(window.aumid.c_str(), box.aumid.c_str()) == 0;
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
    const bool aumidMatch = SameAumid(window, box);
    if (!aumidMatch && !SameExe(window, box)) return 0;

    int score = aumidMatch ? 300 : kExeOnlyScore;

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
        if (!SameAumid(candidate, box) && !SameExe(candidate, box)) continue;
        if (SameExe(candidate, box)) ++result.sameExeCount;

        const int score = ScoreWindowForBox(candidate, box, sticky);
        const int area = (candidate.rect.right - candidate.rect.left) *
                         (candidate.rect.bottom - candidate.rect.top);

        if (score > result.score) {
            result.score = score;
            result.hwnd = candidate.hwnd;
        } else if (score == result.score && score > 0 && result.hwnd) {
            RECT curRect = candidate.rect;
            for (const auto& w : windows) {
                if (w.hwnd == result.hwnd) {
                    curRect = w.rect;
                    break;
                }
            }
            const int curArea = (curRect.right - curRect.left) * (curRect.bottom - curRect.top);
            if (area > curArea) {
                result.hwnd = candidate.hwnd;
            }
        } else if (score == result.score && score > 0 && !result.hwnd) {
            result.hwnd = candidate.hwnd;
        }
    }

    // Sticky HWND still alive but missing from the enum (cloaked / filter race) — reuse it.
    if (!result.hwnd && sticky && IsWindow(sticky) && !usedWindows.count(sticky)) {
        WindowInfo stickyInfo;
        for (const auto& candidate : WindowScaler::GetActiveWindows()) {
            if (candidate.hwnd == sticky) {
                stickyInfo = candidate;
                break;
            }
        }
        if (SameAumid(stickyInfo, box) || SameExe(stickyInfo, box)) {
            result.hwnd = sticky;
            result.score = 1000 + kMinReuseScore;
            if (result.sameExeCount == 0) result.sameExeCount = 1;
        }
    }

    // Weak sticky with multiple siblings (e.g. wrong Explorer) — re-match by title only.
    if (sticky && result.sameExeCount > 1 && result.hwnd == sticky &&
        (result.score - 1000) < kMinReuseScore) {
        result.score = 0;
        result.hwnd = nullptr;
        for (const auto& candidate : windows) {
            if (usedWindows.count(candidate.hwnd)) continue;
            if (!SameAumid(candidate, box) && !SameExe(candidate, box)) continue;
            const int score = ScoreWindowForBox(candidate, box, nullptr);
            const int area = (candidate.rect.right - candidate.rect.left) *
                             (candidate.rect.bottom - candidate.rect.top);
            if (score > result.score) {
                result.score = score;
                result.hwnd = candidate.hwnd;
            } else if (score == result.score && score > 0 && result.hwnd) {
                RECT curRect = candidate.rect;
                for (const auto& w : windows) {
                    if (w.hwnd == result.hwnd) { curRect = w.rect; break; }
                }
                const int curArea = (curRect.right - curRect.left) * (curRect.bottom - curRect.top);
                if (area > curArea) result.hwnd = candidate.hwnd;
            }
        }
    }

    return result;
}

bool ShouldReuseExisting(const MatchResult& match, const SelectedBox& box) {
    if (!match.hwnd || match.score <= 0) return false;

    const std::string exe = ExpectedExe(box);

    if (AppLauncher::IsObsidianExe(exe)) {
        if (match.sameExeCount == 1) return true;
        return match.score >= kMinReuseScore;
    }

    // explorer.exe: never weak-match when multiple File Explorer windows are open.
    if (WindowScaler::IsExplorerProcess(exe) &&
        match.sameExeCount > 1 && match.score < kMinReuseScore) {
        return false;
    }

    if (match.score >= kMinReuseScore) return true;

    if (match.score <= kExeOnlyScore && match.sameExeCount > 1) return false;

    if (match.stickyDead && match.score <= kExeOnlyScore) return false;

    return match.sameExeCount == 1;
}

bool ResolveZoneMonitor(const SelectedBox& box, int& outMonitorIndex, std::string& skipReason) {
    MonitorBoxRef ref{};
    ref.stableMonitorId = box.stableMonitorId;
    ref.monitorDevice = box.monitorDevice;
    ref.monitorIndex = box.monitorIndex;

    const MonitorResolveResult resolved = MonitorManager::ResolveMonitorForBox(ref);
    if (resolved.resolvedIndex < 0) {
        skipReason = resolved.skipReason.empty() ? "monitor disconnected" : resolved.skipReason;
        return false;
    }

    outMonitorIndex = resolved.resolvedIndex;
    return true;
}

void SendMonitorsChangedToUi() {
    const std::string payload = MonitorManager::SerializeMonitorsJson();
    WebViewWindow::SendMessageToUI(
        std::string("{\"action\":\"MONITORS_CHANGED\",\"payload\":") + payload + "}"
    );
}

void ClearStickyHwnds() {
    g_stickyHwnds.clear();
}

void NotifyActiveBiomeChanged() {
    WebViewWindow::SendMessageToUI(
        "{\"action\":\"ACTIVE_BIOME_CHANGED\",\"id\":\"" + EscapeJsonString(g_activeBiomeId) + "\"}"
    );
}

void PumpUiMessagesBriefly() {
    MSG msg{};
    while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            PostQuitMessage(static_cast<int>(msg.wParam));
            return;
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
}

bool DeactivateActiveBiome(std::string& status) {
    if (g_activeBiomeId.empty()) {
        status = "No Biome is currently open.";
        return false;
    }

    WindowScaler::CloseBiomeSession();
    WebViewWindow::RestoreDashboard();
    g_activeBiomeId.clear();
    status = "Biome closed. Apps minimized; other windows stay hidden.";
    WriteRuntimeLog("[APP] " + status);
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

    // Switching or refreshing: close current biome session cleanly.
    if (!g_activeBiomeId.empty()) {
        WindowScaler::CloseBiomeSession();
        ClearStickyHwnds();
        g_activeBiomeId.clear();
    }

    const std::vector<SelectedBox> layout = JsonManager::SelectLayoutForTopology(*profile);

    std::vector<WindowInfo> activeWindows = WindowScaler::GetActiveWindows();
    std::unordered_set<HWND> keepVisible;
    std::unordered_set<HWND> tentativelyUsed;

    for (const auto& box : layout) {
        if (box.assignedApp.empty()) continue;
        int monitorIndex = box.monitorIndex;
        std::string skipReason;
        if (!ResolveZoneMonitor(box, monitorIndex, skipReason)) continue;

        SelectedBox resolvedBox = box;
        resolvedBox.monitorIndex = monitorIndex;

        const MatchResult match = FindBestWindowForBox(resolvedBox, activeWindows, tentativelyUsed);
        if (ShouldReuseExisting(match, resolvedBox) && match.hwnd) {
            keepVisible.insert(match.hwnd);
            tentativelyUsed.insert(match.hwnd);
        }
    }

    WebViewWindow::MinimizeDashboard();
    WindowScaler::PrepareCleanSlate(WebViewWindow::GetHwnd(), keepVisible);
    PumpUiMessagesBriefly();
    Sleep(80);
    PumpUiMessagesBriefly();

    activeWindows = WindowScaler::GetActiveWindows();
    std::unordered_set<HWND> usedWindows;
    std::vector<HWND> placedBiomeHwnds;
    std::vector<std::pair<HWND, SelectedBox>> placedPairs;
    size_t placed = 0;
    size_t launched = 0;
    size_t pendingLaunches = 0;
    size_t failed = 0;
    size_t skippedUwp = 0;
    size_t skippedMonitor = 0;
    std::vector<std::string> zoneNotes;

    size_t totalZones = 0;
    for (const auto& box : layout) {
        if (!box.assignedApp.empty()) ++totalZones;
    }

    for (auto box : layout) {
        if (box.assignedApp.empty()) continue;

        std::string skipReason;
        if (!ResolveZoneMonitor(box, box.monitorIndex, skipReason)) {
            ++skippedMonitor;
            zoneNotes.push_back(ExpectedExe(box) + ": skipped (" + skipReason + ")");
            continue;
        }

        const std::string label = !ExpectedExe(box).empty() ? ExpectedExe(box) : box.assignedApp;

        if (WindowScaler::IsUnsupportedUwpBinding(box.assignedApp)) {
            ++skippedUwp;
            ++failed;
            zoneNotes.push_back(label + ": skipped (UWP/ApplicationFrameHost)");
            continue;
        }

        const MatchResult match = FindBestWindowForBox(box, activeWindows, usedWindows);

        if (ShouldReuseExisting(match, box)) {
            WindowScaler::CacheBiomeAppPreState(match.hwnd, false);
            if (WindowScaler::ForceSnapToBox(match.hwnd, box)) {
                usedWindows.insert(match.hwnd);
                placedBiomeHwnds.push_back(match.hwnd);
                placedPairs.emplace_back(match.hwnd, box);
                g_stickyHwnds[box.id] = match.hwnd;
                ++placed;
                zoneNotes.push_back(label + ": placed existing (score " + std::to_string(match.score) + ")");
            } else {
                ++failed;
                zoneNotes.push_back(label + ": snap failed");
            }
            continue;
        }

        std::unordered_set<HWND> exclude = usedWindows;
        for (const auto& win : activeWindows) {
            exclude.insert(win.hwnd);
        }

        // Obsidian: never bare-launch; reuse or URI-only path in LaunchAndSnapApp.
        if (AppLauncher::IsObsidianExe(label)) {
            const std::string resolvedUri = AppLauncher::ResolveObsidianLaunchUri(box);
            if (match.sameExeCount > 0) {
                ++failed;
                zoneNotes.push_back(label + ": open the correct vault first (title must match)");
                continue;
            }
            if (resolvedUri.empty()) {
                ++failed;
                zoneNotes.push_back(label + ": recreate zone with vault open (no vault resolved)");
                continue;
            }
            box.launchUri = resolvedUri;
        }

        // Packaged/Store app without resolvable AUMID — skip instead of blocking error dialog.
        if ((AppLauncher::IsPackagedAppPath(box.assignedApp) || !box.aumid.empty()) &&
            AppLauncher::ResolveAumidCandidates(box).empty()) {
            ++failed;
            zoneNotes.push_back(label + ": Store app — recreate zone while app is open");
            continue;
        }

        // Explorer: minimize extra windows before launching another copy.
        if (WindowScaler::IsExplorerProcess(label)) {
            WindowScaler::MinimizeExeSiblings(label, nullptr);
        }

        std::string launchError;
        if (WindowScaler::LaunchAndTrackApp(box.assignedApp, box, exclude, launchError)) {
            ++launched;
            ++pendingLaunches;
            if (match.stickyDead) {
                zoneNotes.push_back(label + ": launching asynchronously (previous window closed)");
            } else if (match.sameExeCount > 1) {
                zoneNotes.push_back(label + ": launching asynchronously (avoided sibling window)");
            } else {
                zoneNotes.push_back(label + ": launching asynchronously");
            }
        } else {
            ++failed;
            zoneNotes.push_back(label + ": failed to launch (" + launchError + ")");
        }
    }

    // Settle pass: re-snap if iconic, wrong monitor, still fullscreen, or far from target.
    if (!placedPairs.empty()) {
        PumpUiMessagesBriefly();
        Sleep(80);
        PumpUiMessagesBriefly();
        for (const auto& entry : placedPairs) {
            HWND hwnd = entry.first;
            const SelectedBox& box = entry.second;
            if (!hwnd || !IsWindow(hwnd)) continue;

            RECT current{};
            RECT work{};
            bool needsResnap = IsIconic(hwnd) != FALSE;
            if (GetWindowRect(hwnd, &current)) {
                HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
                MONITORINFO mi{};
                mi.cbSize = sizeof(mi);
                if (GetMonitorInfo(mon, &mi)) {
                    const int monArea = (mi.rcMonitor.right - mi.rcMonitor.left) *
                                        (mi.rcMonitor.bottom - mi.rcMonitor.top);
                    const int winArea = (current.right - current.left) *
                                        (current.bottom - current.top);
                    if (monArea > 0 && winArea >= static_cast<int>(monArea * 0.85)) {
                        needsResnap = true; // still maximized/fullscreen-sized
                    }
                }
                if (MonitorManager::GetWorkAreaForBox(box.monitorIndex, box.monitorDevice, box.stableMonitorId, work)) {
                    const LONG cx = (current.left + current.right) / 2;
                    const LONG cy = (current.top + current.bottom) / 2;
                    if (cx < work.left || cx >= work.right || cy < work.top || cy >= work.bottom) {
                        needsResnap = true;
                    }
                    const int tw = work.right - work.left;
                    const int th = work.bottom - work.top;
                    const int expectedW = static_cast<int>(box.relWidth * tw);
                    const int expectedH = static_cast<int>(box.relHeight * th);
                    const int aw = current.right - current.left;
                    const int ah = current.bottom - current.top;
                    if (std::abs(aw - expectedW) > 80 || std::abs(ah - expectedH) > 80) {
                        needsResnap = true;
                    }
                }
            }
            if (needsResnap) {
                WindowScaler::ForceSnapToBox(hwnd, box);
            }
            PumpUiMessagesBriefly();
        }
    }

    WindowScaler::RaiseBiomeWindows(placedBiomeHwnds);
    WindowScaler::MinimizeExceptPlaced(placedBiomeHwnds, WebViewWindow::GetHwnd());
    // Keep Biomes minimized on the taskbar — never destroy/hide it during launch.
    WebViewWindow::MinimizeDashboard();

    if (placed > 0 || pendingLaunches > 0) {
        g_activeBiomeId = biomeId;
        NotifyActiveBiomeChanged();
    } else {
        WebViewWindow::RestoreDashboard();
    }
    g_activationInProgress = false;

    std::ostringstream summary;
    if (placed > 0 || pendingLaunches > 0) {
        summary << "Biome opened: " << placed << "/" << totalZones << " placed";
    } else {
        summary << "Biome launch failed: 0/" << totalZones << " placed";
    }
    if (launched > 0) summary << " (" << launched << " newly launched)";
    if (pendingLaunches > 0) summary << "; " << pendingLaunches << " waiting for workspace windows";
    if (failed > 0) summary << ", " << failed << " failed";
    if (skippedUwp > 0) summary << " [UWP slots need a real .exe]";
    if (skippedMonitor > 0) summary << ", " << skippedMonitor << " skipped (monitor)";
    if (!zoneNotes.empty()) {
        summary << " | ";
        for (size_t i = 0; i < zoneNotes.size(); ++i) {
            if (i) summary << "; ";
            summary << zoneNotes[i];
        }
    }
    if (placed > 0 || pendingLaunches > 0) {
        summary << " | Biomes stays on the taskbar — click it or press the hotkey again to close.";
    }

    status = summary.str();
    WriteRuntimeLog("[APP] " + status);
    for (const auto& note : zoneNotes) {
        std::cout << "[PLACE] " << note << std::endl;
    }
    return placed > 0 || pendingLaunches > 0;
}

bool ToggleBiome(const std::string& biomeId, std::string& status) {
    if (!biomeId.empty() && g_activeBiomeId == biomeId) {
        return DeactivateActiveBiome(status);
    }
    return ActivateBiome(biomeId, status);
}

void SyncHotkeysFromDisk() {
    if (g_recordingHotkey) return;
    std::vector<BiomeProfile> profiles;
    JsonManager::LoadBiomesFromFile(GetBiomesConfigPath().string(), profiles);
    HotkeyManager::SyncBiomeHotkeys(WebViewWindow::GetHwnd(), profiles);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

#ifdef _DEBUG
    AllocConsole();
    freopen_s((FILE**)stdout, "CONOUT$", "w", stdout);
    freopen_s((FILE**)stderr, "CONOUT$", "w", stderr);
#endif

    std::cout << "=== Biomes Workspace Engine Active ===" << std::endl;
    WriteRuntimeLog("[APP] Startup begin");

    GridOverlay::SetCompletedCallback([](const std::vector<SelectedBox>& boxes) {
        WebViewWindow::RestoreDashboard();
        try {
        json payload;
        payload["action"] = "GRID_LAYOUT_READY";
        payload["boxes"] = json::array();
        for (const auto& box : boxes) payload["boxes"].push_back(SerializeBox(box));
        WebViewWindow::SendMessageToUI(payload.dump());
        WriteRuntimeLog("[APP] GRID_LAYOUT_READY sent with " + std::to_string(boxes.size()) + " boxes");
        } catch (const std::exception& error) {
            WriteRuntimeLog(std::string("[APP] Layout handoff failed: ") + error.what());
            WebViewWindow::SendMessageToUI(R"({"action":"STATUS","payload":"Could not open the save dialog. Your saved biomes are unchanged. Please try creating the layout again."})");
        }
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

            if (action == "HOTKEY_RECORDING") {
                g_recordingHotkey = request.value("recording", false);
                if (g_recordingHotkey) HotkeyManager::Clear(WebViewWindow::GetHwnd());
                else SyncHotkeysFromDisk();
            }
            else if (action == "WINDOW_CONTROL") {
                const auto command = request.value("command", "");
                const HWND dashboard = WebViewWindow::GetHwnd();
                if (command == "minimize") WebViewWindow::MinimizeDashboard();
                else if (command == "maximize") ShowWindow(dashboard, IsZoomed(dashboard) ? SW_RESTORE : SW_MAXIMIZE);
                else if (command == "close") PostMessage(dashboard, WM_CLOSE, 0, 0);
                else if (command == "resize" && !IsZoomed(dashboard)) {
                    const std::string edge = request.value("edge", "");
                    const std::unordered_map<std::string, WPARAM> edges = {
                        {"w", WMSZ_LEFT}, {"e", WMSZ_RIGHT}, {"n", WMSZ_TOP},
                        {"s", WMSZ_BOTTOM}, {"nw", WMSZ_TOPLEFT}, {"ne", WMSZ_TOPRIGHT},
                        {"sw", WMSZ_BOTTOMLEFT}, {"se", WMSZ_BOTTOMRIGHT}
                    };
                    const auto found = edges.find(edge);
                    if (found != edges.end()) {
                        ReleaseCapture();
                        PostMessage(dashboard, WM_SYSCOMMAND, SC_SIZE | found->second, 0);
                    }
                }
                else if (command == "drag") {
                    ReleaseCapture();
                    PostMessage(dashboard, WM_NCLBUTTONDOWN, HTCAPTION, 0);
                }
            }
            else if (action == "OPEN_EXTERNAL") {
                const auto url = request.value("url", "");
                if (url.rfind("https://github.com/", 0) == 0 || url.rfind("https://www.reddit.com/", 0) == 0) {
                    ShellExecuteA(WebViewWindow::GetHwnd(), "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                }
            }
            else if (action == "CREATE_NEW_BIOME" || action == "SHOW_DESKTOP") {
                WriteRuntimeLog("[APP] CREATE_NEW_BIOME — hide dashboard, fullscreen overlay");
                WindowScaler::PrepareForOverlayCreate(WebViewWindow::GetHwnd());
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
                         << "\"path\":\"" << EscapeJsonString(appPath) << "\","
                         << "\"aumid\":\"" << EscapeJsonString(windows[i].aumid) << "\""
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

                if (!profile.hotkey.empty()) {
                    UINT modifiers = 0, key = 0;
                    if (!HotkeyManager::ParseHotkeyString(profile.hotkey, modifiers, key)) {
                        WebViewWindow::SendMessageToUI(R"({"action":"SAVE_FAILED","payload":"Choose a valid shortcut."})");
                        return;
                    }
                    bool ownsShortcut = false;
                    for (const auto& existing : profiles) {
                        UINT oldModifiers = 0, oldKey = 0;
                        if (!HotkeyManager::ParseHotkeyString(existing.hotkey, oldModifiers, oldKey)) continue;
                        if (oldModifiers != modifiers || oldKey != key) continue;
                        if (existing.id != profile.id) {
                            WebViewWindow::SendMessageToUI(R"({"action":"SAVE_FAILED","payload":"Another biome already uses this shortcut."})");
                            return;
                        }
                        ownsShortcut = true;
                    }
                    if (!ownsShortcut) {
                        constexpr int validationId = 0xBFFE;
                        if (!RegisterHotKey(WebViewWindow::GetHwnd(), validationId, modifiers | MOD_NOREPEAT, key)) {
                            WebViewWindow::SendMessageToUI(R"({"action":"SAVE_FAILED","payload":"Windows or another app uses this shortcut. Choose a different one."})");
                            return;
                        }
                        UnregisterHotKey(WebViewWindow::GetHwnd(), validationId);
                    }
                }
                bool replaced = false;
                for (auto& existing : profiles) {
                    if (existing.id == profile.id) {
                        profile.layoutVariants = existing.layoutVariants;
                        JsonManager::EnrichProfileForSave(profile);
                        existing = profile;
                        replaced = true;
                        break;
                    }
                }
                if (!replaced) {
                    JsonManager::EnrichProfileForSave(profile);
                    profiles.push_back(profile);
                }

                std::filesystem::create_directories(configPath.parent_path());
                if (!JsonManager::SaveBiomesToFile(configPath.string(), profiles)) {
                    WebViewWindow::SendMessageToUI(R"({"action":"STATUS","payload":"Could not save this Biome."})");
                    return;
                }

                SyncHotkeysFromDisk();
                SendSavedBiomesToUi();
                WebViewWindow::SendMessageToUI(json({{"action","BIOME_SAVED"},{"id",profile.id}}).dump());
            }
            else if (action == "FIX_BIOME_LAYOUT") {
                const std::string biomeId = request.value("id", "");
                if (biomeId.empty()) {
                    WebViewWindow::SendMessageToUI(R"({"action":"STATUS","payload":"Missing Biome id."})");
                    return;
                }

                std::vector<BiomeProfile> profiles;
                if (!JsonManager::LoadBiomesFromFile(GetBiomesConfigPath().string(), profiles)) {
                    WebViewWindow::SendMessageToUI(R"({"action":"STATUS","payload":"Could not read saved Biomes."})");
                    return;
                }

                const auto profile = std::find_if(profiles.begin(), profiles.end(), [&](const BiomeProfile& item) {
                    return item.id == biomeId;
                });
                if (profile == profiles.end()) {
                    WebViewWindow::SendMessageToUI(R"({"action":"STATUS","payload":"Biome not found."})");
                    return;
                }

                const std::vector<SelectedBox> remapped =
                    JsonManager::RemapLayoutToCurrentMonitors(profile->layout);

                WriteRuntimeLog("[APP] FIX_BIOME_LAYOUT — hide dashboard, repair overlay");
                WindowScaler::PrepareForOverlayCreate(WebViewWindow::GetHwnd());
                WebViewWindow::HideDashboard();
                Sleep(150);

                if (!GridOverlay::ShowOverlayWithLayout(remapped)) {
                    WebViewWindow::RestoreDashboard();
                    WebViewWindow::SendMessageToUI(R"({"action":"STATUS","payload":"Could not open the repair overlay."})");
                }
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

    WebViewWindow::SetDisplayChangedCallback([]() {
        WriteRuntimeLog("[APP] Display or work-area change detected");
        SendMonitorsChangedToUi();
    });
    SendMonitorsChangedToUi();

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
        {"stableMonitorId", box.stableMonitorId},
        {"topologyHash", box.topologyHash},
        {"aumid", box.aumid},
        {"launchUri", box.launchUri}
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
    box.stableMonitorId = value.value("stableMonitorId", "");
    box.topologyHash = value.value("topologyHash", "");
    box.aumid = value.value("aumid", "");
    box.launchUri = value.value("launchUri", "");
    if (box.exeName.empty() && !box.assignedApp.empty()) {
        box.exeName = ExecutableName(box.assignedApp);
    }
    return box;
}

void SendSavedBiomesToUi() {
    std::vector<BiomeProfile> checkedProfiles;
    if (!JsonManager::LoadBiomesFromFile(GetBiomesConfigPath().string(), checkedProfiles)) {
        WebViewWindow::SendMessageToUI(R"({"action":"LOAD_FAILED","payload":"Could not read saved biomes. Your file has not been changed."})");
        return;
    }
    const std::string biomes = JsonManager::LoadBiomesAsJsonString(GetBiomesConfigPath().string());
    WebViewWindow::SendMessageToUI(
        "{\"action\":\"LOADED_BIOMES\",\"biomes\":" + biomes +
        ",\"activeId\":\"" + EscapeJsonString(g_activeBiomeId) + "\"}"
    );
}
