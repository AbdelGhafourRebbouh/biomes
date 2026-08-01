#include "../../include/core/monitor_manager.hpp"
using namespace std;

// Win32 Monitor Enumeration Callback
BOOL CALLBACK MonitorEnumCallback(HMONITOR hMonitor, HDC hdcMonitor, LPRECT lprcMonitor, LPARAM dwData) {
    auto* monitors = reinterpret_cast<std::vector<MonitorDetail>*>(dwData);

    MONITORINFOEXA mi;
    mi.cbSize = sizeof(MONITORINFOEXA);

    if (GetMonitorInfoA(hMonitor, &mi)) {
        MonitorDetail detail;
        detail.deviceName = mi.szDevice;
        detail.rect = mi.rcWork; // rcWork excludes taskbars for clean grid fitting
        detail.width = mi.rcWork.right - mi.rcWork.left;
        detail.height = mi.rcWork.bottom - mi.rcWork.top;
        detail.isPrimary = (mi.dwFlags & MONITORINFOF_PRIMARY);

        monitors->push_back(detail);
    }
    return TRUE;
}

vector<MonitorDetail> MonitorManager::GetConnectedMonitors() {
    vector<MonitorDetail> monitors;
    EnumDisplayMonitors(NULL, NULL, MonitorEnumCallback, reinterpret_cast<LPARAM>(&monitors));
    return monitors;
}

bool MonitorManager::GetMonitorByName(const string& deviceName, MonitorDetail& outMonitor) {
    auto monitors = GetConnectedMonitors();
    for (const auto& mon : monitors) {
        if (mon.deviceName == deviceName) {
            outMonitor = mon;
            return true;
        }
    }
    return false;
}