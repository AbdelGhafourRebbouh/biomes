#pragma once
#include <windows.h>
#include <string>
#include <vector>

struct MonitorDetail {
    int index = 0;
    std::string deviceName; // e.g. \\.\DISPLAY1
    HMONITOR hMonitor = nullptr;
    RECT rcWork{};          // work area (excludes taskbar) — used for overlay + snap
    RECT rcMonitor{};       // full monitor bounds
    int width = 0;
    int height = 0;
    bool isPrimary = false;
};

class MonitorManager {
public:
    // Single source of truth for monitor enumeration (stable index order).
    static std::vector<MonitorDetail> GetConnectedMonitors();

    static bool GetMonitorByName(const std::string& deviceName, MonitorDetail& outMonitor);

    // Resolve saved monitorDevice to current index; returns -1 if disconnected.
    static int ResolveMonitorIndex(const std::string& monitorDevice, int fallbackIndex);

    // Work-area rect for a zone after resolving monitor index; false if invalid/disconnected.
    static bool GetWorkAreaForBox(int monitorIndex, const std::string& monitorDevice, RECT& outWork);
};
