#pragma once
#include <windows.h>
#include <string>
#include <vector>

struct MonitorDetail {
    std::string deviceName; // e.g., "\\.\DISPLAY1"
    RECT rect;              // Bounds (left, top, right, bottom)
    int width;
    int height;
    bool isPrimary;
};

class MonitorManager {
public:
    // Enumerates all connected displays and returns their hardware details and RECT bounds
    static std::vector<MonitorDetail> GetConnectedMonitors();

    // Helper to find a specific monitor by its device name
    static bool GetMonitorByName(const std::string& deviceName, MonitorDetail& outMonitor);
};