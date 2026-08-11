#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <iostream>
#include <string>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <unordered_set>
#include <algorithm>
#include "../include/external/nlohmann/json.hpp"

#include "../include/ui/webview_window.hpp"
#include "../include/core/window_scaler.hpp"
#include "../include/ui/grid_overlay.hpp"
#include "../include/core/monitor_manager.hpp"
#include "../include/core/json_manager.hpp"
#include "../include/core/biome_manager.hpp"

using json = nlohmann::json;

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

// Helper to sanitize strings for IPC JSON transmission
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

std::string ExecutableName(const std::string& path) {
    return std::filesystem::path(path).filename().string();
}

bool IsSameApplication(const WindowInfo& window, const std::string& assignedApp) {
    const std::string expected = ExecutableName(assignedApp);
    return !expected.empty() && _stricmp(window.processName.c_str(), expected.c_str()) == 0;
}

bool ActivateBiome(const std::string& biomeId, std::string& status) {
    std::vector<BiomeProfile> profiles;
    if (!JsonManager::LoadBiomesFromFile(GetBiomesConfigPath().string(), profiles)) {
        status = "Could not read saved Biomes.";
        return false;
    }

    const auto profile = std::find_if(profiles.begin(), profiles.end(), [&](const BiomeProfile& item) {
        return item.id == biomeId;
    });
    if (profile == profiles.end()) {
        status = "The selected Biome no longer exists.";
        return false;
    }

    WindowScaler::ShowDesktop();
    std::vector<WindowInfo> activeWindows = WindowScaler::GetActiveWindows();
    std::unordered_set<HWND> usedWindows;
    size_t placed = 0;
    size_t failed = 0;

    for (const auto& box : profile->layout) {
        if (box.assignedApp.empty()) continue;

        const auto window = std::find_if(activeWindows.begin(), activeWindows.end(), [&](const WindowInfo& candidate) {
            return !usedWindows.count(candidate.hwnd) && IsSameApplication(candidate, box.assignedApp);
        });

        if (window != activeWindows.end()) {
            if (WindowScaler::SnapToBox(window->hwnd, box)) {
                usedWindows.insert(window->hwnd);
                ++placed;
            } else {
                ++failed;
            }
        } else if (WindowScaler::LaunchAndSnapApp(box.assignedApp, box)) {
            ++placed;
            activeWindows = WindowScaler::GetActiveWindows();
        } else {
            ++failed;
        }
    }

    status = "Biome launched: " + std::to_string(placed) + " app(s) placed";
    if (failed > 0) status += ", " + std::to_string(failed) + " app(s) could not be placed";
    return failed == 0;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    // Attach debugging console window
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

    // Handle all incoming IPC messages from the Home Dashboard (index.html)
    WebViewWindow::SetMessageReceivedCallback([](const std::string& message) {
        try {
            std::cout << "[IPC RECEIVED RAW]: " << message << std::endl;
            WriteRuntimeLog("[APP] IPC raw message: " + message);
            
            json request = json::parse(message);
            WriteRuntimeLog("[APP] JSON parsed successfully");
            
            if (!request.is_object()) {
                WriteRuntimeLog("[APP] ERROR: parsed JSON is not an object");
                WebViewWindow::SendMessageToUI(R"({"action":"STATUS","payload":"Invalid request format."})");
                return;
            }
            
            std::string action = request.value("action", "");
            WriteRuntimeLog("[APP] Action extracted: " + action);

            // 1. Trigger Multi-Monitor Grid Overlay on Wallpaper (Win + D)
            if (action == "CREATE_NEW_BIOME" || action == "SHOW_DESKTOP") {
                std::cout << "[ENGINE] Pressing Windows + D to minimize all windows..." << std::endl;
                WriteRuntimeLog("[APP] CREATE_NEW_BIOME requested - pressing Win+D");
                
                // Simulate Windows + D to show desktop
                INPUT inputs[4] = {};
                inputs[0].type = INPUT_KEYBOARD;
                inputs[0].ki.wVk = VK_LWIN;
                inputs[0].ki.dwFlags = 0;
                
                inputs[1].type = INPUT_KEYBOARD;
                inputs[1].ki.wVk = 'D';
                inputs[1].ki.dwFlags = 0;
                
                inputs[2].type = INPUT_KEYBOARD;
                inputs[2].ki.wVk = 'D';
                inputs[2].ki.dwFlags = KEYEVENTF_KEYUP;
                
                inputs[3].type = INPUT_KEYBOARD;
                inputs[3].ki.wVk = VK_LWIN;
                inputs[3].ki.dwFlags = KEYEVENTF_KEYUP;
                
                SendInput(4, inputs, sizeof(INPUT));
                Sleep(500); // Wait for windows to minimize
                
                std::cout << "[ENGINE] Launching Grid Overlay..." << std::endl;
                WriteRuntimeLog("[APP] Launching grid overlay");
                
                if (!GridOverlay::ShowOverlay()) {
                    WriteRuntimeLog("[APP] Grid overlay could not be opened");
                    WebViewWindow::RestoreDashboard();
                    WebViewWindow::SendMessageToUI(R"({"action":"STATUS","payload":"Could not open the grid overlay."})");
                }
            }
            // 2. Query running taskbar windows for drag-and-drop tile matching
            else if (action == "GET_ACTIVE_WINDOWS") {
            auto windows = WindowScaler::GetActiveWindows();
            
            std::ostringstream json;
            json << "{\"action\":\"ACTIVE_WINDOWS_LIST\", \"count\":" << windows.size() << ", \"windows\":[";
            for (size_t i = 0; i < windows.size(); ++i) {
                json << "{"
                     << "\"hwnd\":" << (uintptr_t)windows[i].hwnd << ","
                     << "\"title\":\"" << EscapeJsonString(windows[i].title) << "\","
                     << "\"process\":\"" << EscapeJsonString(windows[i].processName) << "\""
                     << "}";
                if (i + 1 < windows.size()) json << ",";
            }
            json << "]}";

            WebViewWindow::SendMessageToUI(json.str());
        }
        // 3. Restore cached original window bounds
        else if (action == "RESTORE_ALL") {
            WindowScaler::RestoreAllCapturedWindows();
            WebViewWindow::SendMessageToUI(R"({"action":"STATUS", "payload":"All windows restored."})");
        }
        // 4. Send saved Biomes list to Home Dashboard
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
            for (const auto& value : request["boxes"]) profile.layout.push_back(DeserializeBox(value));

            std::vector<BiomeProfile> profiles;
            const auto configPath = GetBiomesConfigPath();
            if (!JsonManager::LoadBiomesFromFile(configPath.string(), profiles)) {
                WebViewWindow::SendMessageToUI(R"({"action":"STATUS","payload":"Could not read saved Biomes."})");
                return;
            }
            bool replaced = false;
            for (auto& existing : profiles) {
                if (existing.id == profile.id) { existing = profile; replaced = true; break; }
            }
            if (!replaced) profiles.push_back(profile);
            std::filesystem::create_directories(configPath.parent_path());
            if (!JsonManager::SaveBiomesToFile(configPath.string(), profiles)) {
                WebViewWindow::SendMessageToUI(R"({"action":"STATUS","payload":"Could not save this Biome."})");
                return;
            }
            SendSavedBiomesToUi();
            WebViewWindow::SendMessageToUI(R"({"action":"BIOME_SAVED"})");
        }
        else if (action == "ACTIVATE_BIOME") {
            const std::string biomeId = request.value("id", "");
            std::string status;
            const bool success = ActivateBiome(biomeId, status);
            WebViewWindow::SendMessageToUI("{\"action\":\"STATUS\",\"payload\":\"" + EscapeJsonString(status) + "\",\"success\":" + (success ? "true" : "false") + "}");
        }
        } catch (const std::exception& e) {
            WriteRuntimeLog("[APP] Exception in message callback: " + std::string(e.what()));
            std::cerr << "[ERROR] Exception: " << e.what() << std::endl;
        } catch (...) {
            WriteRuntimeLog("[APP] Unknown exception in message callback");
            std::cerr << "[ERROR] Unknown exception" << std::endl;
        }
    });

    // Locate index.html alongside the executable
    char buffer[MAX_PATH];
    GetModuleFileNameA(NULL, buffer, MAX_PATH);
    std::filesystem::path exePath(buffer);
    std::filesystem::path htmlPath = exePath.parent_path() / "index.html";
    std::string startUrl = BuildFileUrl(htmlPath);
    WriteRuntimeLog("[APP] Loading dashboard from " + startUrl);

    WriteRuntimeLog("[APP] Initializing WebView2 window");
    // Launch WebView2 Win32 Container
    if (!WebViewWindow::Initialize(hInstance, nCmdShow, startUrl)) {
        WriteRuntimeLog("[APP] WebView2 initialization failed");
        std::cerr << "[ERROR] Failed to initialize WebView2 window." << std::endl;
        return -1;
    }

    WebViewWindow::RunMessageLoop();
    return 0;
}

std::filesystem::path GetBiomesConfigPath() {
    char buffer[MAX_PATH];
    GetModuleFileNameA(NULL, buffer, MAX_PATH);
    return std::filesystem::path(buffer).parent_path() / "config" / "biomes.json";
}

json SerializeBox(const SelectedBox& box) {
    return {{"id", box.id}, {"monitorIndex", box.monitorIndex},
            {"startCol", box.startCol}, {"endCol", box.endCol},
            {"startRow", box.startRow}, {"endRow", box.endRow},
            {"relX", box.relX}, {"relY", box.relY},
            {"relWidth", box.relWidth}, {"relHeight", box.relHeight},
            {"assignedApp", box.assignedApp}};
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
    return box;
}

void SendSavedBiomesToUi() {
    const std::string biomes = JsonManager::LoadBiomesAsJsonString(GetBiomesConfigPath().string());
    WebViewWindow::SendMessageToUI("{\"action\":\"LOADED_BIOMES\",\"biomes\":" + biomes + "}");
}
