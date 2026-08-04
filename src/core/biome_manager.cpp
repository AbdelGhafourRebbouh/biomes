#include "../../include/core/biome_manager.hpp"
#include "../../include/core/window_scaler.hpp"
#include <iostream>
#include <shellapi.h>
#include <windows.h>
#include <thread>
#include <chrono>

using namespace std;

// Initialize static window position cache
std::map<HWND, RECT> BiomeManager::s_OriginalWindowPositions;

// Helper to expand environment strings like %localappdata%
static string ExpandPath(const string& inputPath) {
    char expanded[MAX_PATH];
    DWORD res = ExpandEnvironmentStringsA(inputPath.c_str(), expanded, MAX_PATH);
    if (res > 0 && res <= MAX_PATH) {
        return string(expanded);
    }
    return inputPath;
}

// ----------------------------------------------------------------------
// Windows Registry & Directory App Path Resolver
// ----------------------------------------------------------------------
string BiomeManager::ResolveAppPath(const string& appExe) {
    if (appExe.empty()) return "";

    // 1. If it's already an absolute existing file path, return it directly
    DWORD dwAttrib = GetFileAttributesA(appExe.c_str());
    if (dwAttrib != INVALID_FILE_ATTRIBUTES && !(dwAttrib & FILE_ATTRIBUTE_DIRECTORY)) {
        return appExe;
    }

    // Extract binary name if a full path was provided
    string exeName = appExe;
    size_t lastSlash = exeName.find_last_of("\\/");
    if (lastSlash != string::npos) {
        exeName = exeName.substr(lastSlash + 1);
    }

    // 2. Query Windows Registry (HKLM and HKCU App Paths)
    string regSubKey = "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\" + exeName;
    char pathBuffer[MAX_PATH];
    DWORD bufferSize = sizeof(pathBuffer);
    HKEY hKey;

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, regSubKey.c_str(), 0, KEY_READ, &hKey) == ERROR_SUCCESS ||
        RegOpenKeyExA(HKEY_CURRENT_USER, regSubKey.c_str(), 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        
        if (RegQueryValueExA(hKey, NULL, NULL, NULL, (LPBYTE)pathBuffer, &bufferSize) == ERROR_SUCCESS) {
            RegCloseKey(hKey);
            return string(pathBuffer);
        }
        RegCloseKey(hKey);
    }

    // 3. Fallback: Search common Windows AppData installation folders
    vector<string> fallbackTemplates = {
        "%localappdata%\\Programs\\" + exeName.substr(0, exeName.find_last_of('.')) + "\\" + exeName,
        "%localappdata%\\Programs\\Obsidian\\" + exeName,
        "%programfiles%\\" + exeName,
        "%programfiles(x86)%\\" + exeName
    };

    for (const auto& pathTpl : fallbackTemplates) {
        string testPath = ExpandPath(pathTpl);
        if (GetFileAttributesA(testPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return testPath;
        }
    }

    return appExe;
}

// ----------------------------------------------------------------------
// Generate Matrix Grid targeted to a specific physical monitor
// ----------------------------------------------------------------------
vector<GridBox> BiomeManager::GenerateWindowGridForMonitor(const MonitorDetail& monitor, int rows, int cols, int gap) {
    vector<GridBox> grid;
    
    int totalHorizontalGaps = (cols + 1) * gap;
    int totalVerticalGaps = (rows + 1) * gap;

    int boxWidth = (monitor.width - totalHorizontalGaps) / cols;
    int boxHeight = (monitor.height - totalVerticalGaps) / rows;

    int boxId = 0;

    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            GridBox box;
            box.id = boxId++;
            
            int localX = gap + c * (boxWidth + gap);
            int localY = gap + r * (boxHeight + gap);

            box.x = monitor.rect.left + localX;
            box.y = monitor.rect.top + localY;
            box.width = boxWidth;
            box.height = boxHeight;

            box.relX = static_cast<float>(localX) / monitor.width;
            box.relY = static_cast<float>(localY) / monitor.height;
            box.relWidth = static_cast<float>(boxWidth) / monitor.width;
            box.relHeight = static_cast<float>(boxHeight) / monitor.height;

            box.monitorDeviceName = monitor.deviceName;
            box.assignedAppPath = "";
            box.assignedAppTitle = "";

            grid.push_back(box);
        }
    }

    return grid;
}

// ----------------------------------------------------------------------
// Multi-Monitor Window Snapping & Auto-Launch Engine
// ----------------------------------------------------------------------
void BiomeManager::ApplyLayout(const vector<GridBox>& layout) {
    bool launchedAnyApp = false;

    // Helper lambda to scan active windows and snap matching grid boxes
    auto PerformSnappingPass = [&](const vector<GridBox>& gridLayout) {
        vector<WindowInfo> activeWindows = WindowScaler::GetActiveWindows();

        for (const auto& box : gridLayout) {
            if (box.assignedAppPath.empty() && box.assignedAppTitle.empty()) continue;

            string exeName = box.assignedAppPath;
            size_t lastSlash = exeName.find_last_of("\\/");
            if (lastSlash != string::npos) {
                exeName = exeName.substr(lastSlash + 1);
            }

            for (const auto& win : activeWindows) {
                bool matches = false;

                if (!exeName.empty() && !win.processName.empty()) {
                    if (_stricmp(win.processName.c_str(), exeName.c_str()) == 0) {
                        matches = true;
                    }
                } else if (!box.assignedAppTitle.empty()) {
                    if (win.title.find(box.assignedAppTitle) != string::npos) {
                        matches = true;
                    }
                }

                if (matches) {
                    // Cache original window position before snapping (if not cached already)
                    if (s_OriginalWindowPositions.find(win.hwnd) == s_OriginalWindowPositions.end()) {
                        RECT rect;
                        if (GetWindowRect(win.hwnd, &rect)) {
                            s_OriginalWindowPositions[win.hwnd] = rect;
                        }
                    }

                    if (IsIconic(win.hwnd) || IsZoomed(win.hwnd)) {
                        ShowWindow(win.hwnd, SW_RESTORE);
                    } else {
                        ShowWindow(win.hwnd, SW_SHOW);
                    }

                    WindowScaler::SetPosition(win.hwnd, box.x, box.y, box.width, box.height);

                    cout << "  [SNAPPED] " << win.processName 
                         << " -> Monitor (" << box.monitorDeviceName 
                         << ") Box " << box.id 
                         << " at Pos(" << box.x << ", " << box.y << ")" << endl;
                    break;
                }
            }
        }
    };

    // PASS 1: Snap running applications
    PerformSnappingPass(layout);

    // AUTO-LAUNCH CHECK: Launch missing apps via Registry-resolved path
    vector<WindowInfo> activeWindows = WindowScaler::GetActiveWindows();
    for (const auto& box : layout) {
        if (box.assignedAppPath.empty() && box.assignedAppTitle.empty()) continue;

        string exeName = box.assignedAppPath;
        size_t lastSlash = exeName.find_last_of("\\/");
        if (lastSlash != string::npos) {
            exeName = exeName.substr(lastSlash + 1);
        }

        bool running = false;
        for (const auto& win : activeWindows) {
            if (_stricmp(win.processName.c_str(), exeName.c_str()) == 0) {
                running = true;
                break;
            }
        }

        if (!running && !box.assignedAppPath.empty()) {
            // Resolve path through Windows Registry dynamically
            string resolvedPath = ResolveAppPath(box.assignedAppPath);
            cout << "  [AUTO-LAUNCHING] " << resolvedPath << " for Box " << box.id << "..." << endl;
            
            HINSTANCE result = ShellExecuteA(NULL, "open", resolvedPath.c_str(), NULL, NULL, SW_SHOWNORMAL);
            if ((INT_PTR)result > 32) {
                launchedAnyApp = true;
            }
        }
    }

    // PASS 2: Deferred snap buffer for launched apps
    if (launchedAnyApp) {
        cout << "  [WAITING] Allowing launched applications to initialize window handles..." << endl;
        this_thread::sleep_for(chrono::milliseconds(1500));
        PerformSnappingPass(layout);
    }
}

// ----------------------------------------------------------------------
// Restore Layout: Put windows back to pre-snapped sizes or minimize
// ----------------------------------------------------------------------
void BiomeManager::RestoreLayout(const vector<GridBox>& layout) {
    vector<WindowInfo> activeWindows = WindowScaler::GetActiveWindows();

    for (const auto& box : layout) {
        string exeName = box.assignedAppPath;
        size_t lastSlash = exeName.find_last_of("\\/");
        if (lastSlash != string::npos) exeName = exeName.substr(lastSlash + 1);

        for (const auto& win : activeWindows) {
            if (!exeName.empty() && _stricmp(win.processName.c_str(), exeName.c_str()) == 0) {
                auto it = s_OriginalWindowPositions.find(win.hwnd);
                if (it != s_OriginalWindowPositions.end()) {
                    RECT orig = it->second;
                    int width = orig.right - orig.left;
                    int height = orig.bottom - orig.top;
                    WindowScaler::SetPosition(win.hwnd, orig.left, orig.top, width, height);
                    cout << "  [RESTORED] " << win.processName << " back to original size." << endl;
                } else {
                    ShowWindow(win.hwnd, SW_MINIMIZE);
                }
            }
        }
    }
}