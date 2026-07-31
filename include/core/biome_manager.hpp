#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <utility>
#include "monitor_manager.hpp"

struct GridBox {
    int id;
    
    // Absolute Pixel Coordinates (for live positioning on screen)
    int x;
    int y;
    int width;
    int height;

    // Normalized Relative Coordinates (0.0f to 1.0f) for JSON persistence
    float relX = 0.0f;
    float relY = 0.0f;
    float relWidth = 0.0f;
    float relHeight = 0.0f;

    std::string assignedAppPath;  
    std::string assignedAppTitle; 
    std::string monitorDeviceName; // e.g. "\\.\DISPLAY1"
};

enum class SplitDirection { NONE, HORIZONTAL, VERTICAL };

struct TreeNode {
    GridBox box;
    SplitDirection split = SplitDirection::NONE;
    TreeNode* left = nullptr;
    TreeNode* right = nullptr;
};

class BiomeManager {
public:
    // Generate Matrix Grid specifically for a targeted monitor
    static std::vector<GridBox> GenerateWindowGridForMonitor(const MonitorDetail& monitor, int rows, int cols, int gap);

    // Apply layout positions across target monitors
    static void ApplyLayout(const std::vector<GridBox>& layout);
};