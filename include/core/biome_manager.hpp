#pragma once
#include <string>
#include <vector>
#include <map>
#include <windows.h>
#include "monitor_manager.hpp"

struct GridBox {
    int id;
    int x;
    int y;
    int width;
    int height;
    float relX;
    float relY;
    float relWidth;
    float relHeight;
    std::string monitorDeviceName;
    std::string assignedAppPath;
    std::string assignedAppTitle;
};

class BiomeManager {
public:
    static std::vector<GridBox> GenerateWindowGridForMonitor(const MonitorDetail& monitor, int rows, int cols, int gap);
    static void ApplyLayout(const std::vector<GridBox>& layout);
    static void RestoreLayout(const std::vector<GridBox>& layout);
    
    // Dynamic Executable Resolver via Windows Registry & Fallbacks
    static std::string ResolveAppPath(const std::string& appExe);

private:
    // Caches pre-snapped window positions for layout restoration
    static std::map<HWND, RECT> s_OriginalWindowPositions;
};