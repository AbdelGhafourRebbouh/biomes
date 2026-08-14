#pragma once
#include <windows.h>
#include <string>
#include <vector>

struct MonitorDetail {
    int index = 0;
    std::string deviceName;        // e.g. \\.\DISPLAY1
    std::string stableId;          // EDID-based or GDI fallback
    std::string workAreaSignature; // e.g. 3840x2160@150 (diagnostics)
    std::string friendlyName;      // from DisplayConfig when available
    HMONITOR hMonitor = nullptr;
    RECT rcWork{};                 // work area (excludes taskbar)
    RECT rcMonitor{};              // full monitor bounds
    int width = 0;
    int height = 0;
    bool isPrimary = false;
};

enum class MonitorMatchKind {
    StableId,
    DeviceName,
    Index,
    NotFound
};

struct MonitorBoxRef {
    std::string stableMonitorId;
    std::string monitorDevice;
    int monitorIndex = 0;
};

struct MonitorResolveResult {
    int resolvedIndex = -1;
    MonitorMatchKind matchKind = MonitorMatchKind::NotFound;
    std::string skipReason;
    std::string matchDetail;
};

class MonitorManager {
public:
    static std::vector<MonitorDetail> GetConnectedMonitors();

    static bool GetMonitorByName(const std::string& deviceName, MonitorDetail& outMonitor);
    static bool GetMonitorByStableId(const std::string& stableId, MonitorDetail& outMonitor);

    static int ResolveMonitorIndex(const std::string& monitorDevice, int fallbackIndex);

    // Never remaps missing monitors onto another screen — skipped zones stay skipped.
    static MonitorResolveResult ResolveMonitorForBox(const MonitorBoxRef& box);

    static bool GetWorkAreaForBox(int monitorIndex,
                                   const std::string& monitorDevice,
                                   const std::string& stableMonitorId,
                                   RECT& outWork);

    static std::string BuildStableMonitorId(HMONITOR hMonitor, const std::string& deviceName);
    static std::string GetCurrentTopologyHash();
    static std::string BuildWorkAreaSignature(const MonitorDetail& monitor);

    // JSON: {topologyHash, monitors:[{stableId,deviceName,isPrimary,workW,workH,friendlyName,signature}]}
    static std::string SerializeMonitorsJson();

    struct BiomeMonitorHealth {
        int connectedMonitors = 0;
        int requiredMonitors = 0;   // how many screens this biome was designed for
        int connectedRequired = 0;
        int missingZones = 0;
        bool topologyMatch = true;
        std::vector<std::string> warnings;
    };

    static BiomeMonitorHealth EvaluateBiomeMonitorHealth(
        const std::vector<MonitorBoxRef>& zones,
        const std::string& savedTopologyHash);
};
