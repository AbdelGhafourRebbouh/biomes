#include "../../include/core/biome_manager.hpp"
#include "../../include/core/window_scaler.hpp"
#include <iostream>
#include <shellapi.h>
#include <windows.h>
#include <thread>
#include <chrono>

using namespace std;

string ExpandPath(const string& inputPath) {
    char expanded[MAX_PATH];
    DWORD res = ExpandEnvironmentStringsA(inputPath.c_str(), expanded, MAX_PATH);
    if (res > 0 && res <= MAX_PATH) {
        return string(expanded);
    }
    return inputPath;
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

    // Helper lambda to scan windows and snap matching boxes
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

    // PASS 1: Snap all already-running applications
    PerformSnappingPass(layout);

    // AUTO-LAUNCH CHECK: Launch missing apps
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
            string targetPath = ExpandPath(box.assignedAppPath);
            cout << "  [AUTO-LAUNCHING] " << targetPath << " for Box " << box.id << "..." << endl;
            
            HINSTANCE result = ShellExecuteA(NULL, "open", targetPath.c_str(), NULL, NULL, SW_SHOWNORMAL);
            if ((INT_PTR)result <= 32 && !exeName.empty()) {
                string localAppDataPath = ExpandPath("%localappdata%\\Programs\\obsidian\\" + exeName);
                if (_stricmp(exeName.c_str(), "Obsidian.exe") == 0) {
                    ShellExecuteA(NULL, "open", localAppDataPath.c_str(), NULL, NULL, SW_SHOWNORMAL);
                }
            }
            launchedAnyApp = true;
        }
    }

    // PASS 2: If we launched an app, wait 1.5 seconds for window handle to render, then re-snap!
    if (launchedAnyApp) {
        cout << "  [WAITING] Allowing launched applications to initialize window handles..." << endl;
        this_thread::sleep_for(chrono::milliseconds(1500));
        PerformSnappingPass(layout);
    }
}