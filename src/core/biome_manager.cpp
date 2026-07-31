#include "../../include/core/biome_manager.hpp"
#include "../../include/core/window_scaler.hpp"
#include <iostream>

// ----------------------------------------------------------------------
// Generate Matrix Grid targeted to a specific physical monitor
// ----------------------------------------------------------------------
std::vector<GridBox> BiomeManager::GenerateWindowGridForMonitor(const MonitorDetail& monitor, int rows, int cols, int gap) {
    std::vector<GridBox> grid;
    
    int totalHorizontalGaps = (cols + 1) * gap;
    int totalVerticalGaps = (rows + 1) * gap;

    int boxWidth = (monitor.width - totalHorizontalGaps) / cols;
    int boxHeight = (monitor.height - totalVerticalGaps) / rows;

    int boxId = 0;

    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            GridBox box;
            box.id = boxId++;
            
            // Local relative offset inside monitor
            int localX = gap + c * (boxWidth + gap);
            int localY = gap + r * (boxHeight + gap);

            // Absolute screen coordinates (Monitor Origin + Local Offset)
            box.x = monitor.rect.left + localX;
            box.y = monitor.rect.top + localY;
            box.width = boxWidth;
            box.height = boxHeight;

            // Normalized Relative Percentages (0.0 to 1.0)
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
// Multi-Monitor Window Snapping Engine
// ----------------------------------------------------------------------
void BiomeManager::ApplyLayout(const std::vector<GridBox>& layout) {
    std::vector<WindowInfo> activeWindows = WindowScaler::GetActiveWindows();

    for (const auto& box : layout) {
        if (box.assignedAppPath.empty() && box.assignedAppTitle.empty()) continue;

        for (const auto& win : activeWindows) {
            bool matches = false;

            // Match by process name (e.g., "Obsidian.exe")
            if (!box.assignedAppPath.empty() && !win.processName.empty()) {
                if (win.processName == box.assignedAppPath) {
                    matches = true;
                }
            } 
            // Fallback match by window title substring
            else if (!box.assignedAppTitle.empty()) {
                if (win.title.find(box.assignedAppTitle) != std::string::npos) {
                    matches = true;
                }
            }

            if (matches) {
                // Restore window if minimized or maximized
                if (IsIconic(win.hwnd) || IsZoomed(win.hwnd)) {
                    ShowWindow(win.hwnd, SW_RESTORE);
                } else {
                    ShowWindow(win.hwnd, SW_SHOW);
                }

                // Snap window into position
                WindowScaler::SetPosition(win.hwnd, box.x, box.y, box.width, box.height);

                std::cout << "  [SNAPPED] " << win.processName 
                          << " -> Monitor (" << box.monitorDeviceName 
                          << ") Box " << box.id 
                          << " at Pos(" << box.x << ", " << box.y << ")" << std::endl;
                break; // Move to next layout box
            }
        }
    }
}