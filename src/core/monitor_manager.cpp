#include "../../include/core/monitor_manager.hpp"
using namespace std;

namespace {

BOOL CALLBACK MonitorEnumCallback(HMONITOR hMonitor, HDC, LPRECT, LPARAM dwData) {
    auto* monitors = reinterpret_cast<vector<MonitorDetail>*>(dwData);

    MONITORINFOEXA mi{};
    mi.cbSize = sizeof(MONITORINFOEXA);
    if (!GetMonitorInfoA(hMonitor, &mi)) return TRUE;

    MonitorDetail detail;
    detail.index = static_cast<int>(monitors->size());
    detail.deviceName = mi.szDevice;
    detail.hMonitor = hMonitor;
    detail.rcWork = mi.rcWork;
    detail.rcMonitor = mi.rcMonitor;
    detail.width = mi.rcWork.right - mi.rcWork.left;
    detail.height = mi.rcWork.bottom - mi.rcWork.top;
    detail.isPrimary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;

    monitors->push_back(detail);
    return TRUE;
}

} // namespace

vector<MonitorDetail> MonitorManager::GetConnectedMonitors() {
    vector<MonitorDetail> monitors;
    EnumDisplayMonitors(nullptr, nullptr, MonitorEnumCallback, reinterpret_cast<LPARAM>(&monitors));
    return monitors;
}

bool MonitorManager::GetMonitorByName(const string& deviceName, MonitorDetail& outMonitor) {
    for (const auto& mon : GetConnectedMonitors()) {
        if (mon.deviceName == deviceName) {
            outMonitor = mon;
            return true;
        }
    }
    return false;
}

int MonitorManager::ResolveMonitorIndex(const string& monitorDevice, int fallbackIndex) {
    if (!monitorDevice.empty()) {
        MonitorDetail detail;
        if (GetMonitorByName(monitorDevice, detail)) {
            return detail.index;
        }
        return -1;
    }

    const auto monitors = GetConnectedMonitors();
    if (fallbackIndex >= 0 && fallbackIndex < static_cast<int>(monitors.size())) {
        return fallbackIndex;
    }
    return -1;
}

bool MonitorManager::GetWorkAreaForBox(int monitorIndex, const string& monitorDevice, RECT& outWork) {
    const int resolved = ResolveMonitorIndex(monitorDevice, monitorIndex);
    if (resolved < 0) return false;

    const auto monitors = GetConnectedMonitors();
    if (resolved >= static_cast<int>(monitors.size())) return false;

    outWork = monitors[resolved].rcWork;
    return true;
}
