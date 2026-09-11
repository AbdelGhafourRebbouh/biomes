#include "../../include/core/monitor_manager.hpp"
#include <ShellScalingApi.h>
#include <algorithm>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <iomanip>

using namespace std;

namespace {

string EscapeJson(const string& input) {
    ostringstream ss;
    for (char c : input) {
        switch (c) {
            case '\\': ss << "\\\\"; break;
            case '"':  ss << "\\\""; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20)
                    ss << "\\u00" << hex << setfill('0') << setw(2)
                       << static_cast<unsigned>(static_cast<unsigned char>(c)) << dec;
                else ss << c;
                break;
        }
    }
    return ss.str();
}

uint32_t Fnv1aHash(const string& text) {
    uint32_t hash = 2166136261u;
    for (unsigned char c : text) {
        hash ^= c;
        hash *= 16777619u;
    }
    return hash;
}

string FormatHex4(uint16_t value) {
    ostringstream ss;
    ss << uppercase << hex << setfill('0') << setw(4) << value;
    return ss.str();
}

string WideToUtf8(const wchar_t* wide) {
    if (!wide || !wide[0]) return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    string out(static_cast<size_t>(needed), '\0');
    if (!WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), needed, nullptr, nullptr)) return {};
    out.pop_back();
    return out;
}

struct EdidTargetInfo {
    string stableId;
    string friendlyName;
    POINT position{};
    bool valid = false;
};

map<HMONITOR, EdidTargetInfo> QueryEdidTargetsByMonitor() {
    map<HMONITOR, EdidTargetInfo> result;

    UINT32 pathCount = 0;
    UINT32 modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS ||
        pathCount == 0) {
        return result;
    }

    vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);

    LONG status = QueryDisplayConfig(
        QDC_ONLY_ACTIVE_PATHS,
        &pathCount,
        paths.data(),
        &modeCount,
        modes.data(),
        nullptr);

    if (status == ERROR_INSUFFICIENT_BUFFER) {
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS)
            return result;
        paths.resize(pathCount);
        modes.resize(modeCount);
        status = QueryDisplayConfig(
            QDC_ONLY_ACTIVE_PATHS,
            &pathCount,
            paths.data(),
            &modeCount,
            modes.data(),
            nullptr);
    }

    if (status != ERROR_SUCCESS) return result;

    for (UINT32 i = 0; i < pathCount; ++i) {
        if (!(paths[i].flags & DISPLAYCONFIG_PATH_ACTIVE)) continue;

        DISPLAYCONFIG_SOURCE_MODE* sourceMode = nullptr;
        for (UINT32 m = 0; m < modeCount; ++m) {
            if (modes[m].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE &&
                modes[m].adapterId.HighPart == paths[i].sourceInfo.adapterId.HighPart &&
                modes[m].adapterId.LowPart == paths[i].sourceInfo.adapterId.LowPart &&
                modes[m].id == paths[i].sourceInfo.id) {
                sourceMode = &modes[m].sourceMode;
                break;
            }
        }
        if (!sourceMode) continue;

        POINT pt{ sourceMode->position.x + 1, sourceMode->position.y + 1 };
        HMONITOR hMonitor = MonitorFromPoint(pt, MONITOR_DEFAULTTONULL);
        if (!hMonitor) continue;

        DISPLAYCONFIG_TARGET_DEVICE_NAME targetName{};
        targetName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        targetName.header.size = sizeof(targetName);
        targetName.header.adapterId = paths[i].targetInfo.adapterId;
        targetName.header.id = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&targetName.header) != ERROR_SUCCESS) continue;

        EdidTargetInfo info;
        info.position.x = sourceMode->position.x;
        info.position.y = sourceMode->position.y;
        info.friendlyName = WideToUtf8(targetName.monitorFriendlyDeviceName);

        if (targetName.edidManufactureId != 0 || targetName.edidProductCodeId != 0) {
            info.stableId = "EDID:" + FormatHex4(targetName.edidManufactureId) + "-" +
                            FormatHex4(targetName.edidProductCodeId);
            info.valid = true;
        }

        // Prefer first EDID hit; disambiguate duplicates later via device name.
        if (result.find(hMonitor) == result.end() || (!result[hMonitor].valid && info.valid)) {
            result[hMonitor] = info;
        }
    }

    return result;
}

void DisambiguateDuplicateStableIds(vector<MonitorDetail>& monitors) {
    map<string, vector<int>> groups;
    for (int i = 0; i < static_cast<int>(monitors.size()); ++i) {
        groups[monitors[i].stableId].push_back(i);
    }

    for (const auto& entry : groups) {
        if (entry.second.size() <= 1) continue;
        for (int idx : entry.second) {
            monitors[idx].stableId += "@" + monitors[idx].deviceName;
        }
    }
}

BOOL CALLBACK MonitorEnumCallback(HMONITOR hMonitor, HDC, LPRECT, LPARAM dwData) {
    auto* monitors = reinterpret_cast<vector<MonitorDetail>*>(dwData);

    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hMonitor, &mi)) return TRUE;

    MonitorDetail detail;
    detail.index = static_cast<int>(monitors->size());
    detail.deviceName = WideToUtf8(mi.szDevice);
    detail.hMonitor = hMonitor;
    detail.rcWork = mi.rcWork;
    detail.rcMonitor = mi.rcMonitor;
    detail.width = mi.rcWork.right - mi.rcWork.left;
    detail.height = mi.rcWork.bottom - mi.rcWork.top;
    detail.isPrimary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
    UINT dpiX = 96, dpiY = 96;
    if (SUCCEEDED(GetDpiForMonitor(hMonitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)) && dpiX && dpiY) {
        detail.dpiX = dpiX;
        detail.dpiY = dpiY;
        detail.scaleX = dpiX / 96.0;
        detail.scaleY = dpiY / 96.0;
    }

    monitors->push_back(detail);
    return TRUE;
}

int FindPrimaryIndex(const vector<MonitorDetail>& monitors) {
    for (const auto& mon : monitors) {
        if (mon.isPrimary) return mon.index;
    }
    return monitors.empty() ? -1 : 0;
}

} // namespace

string MonitorManager::BuildStableMonitorId(HMONITOR hMonitor, const string& deviceName) {
    // Use the same duplicate disambiguation as enumeration and persistence.
    for (const auto& monitor : GetConnectedMonitors()) {
        if (monitor.hMonitor == hMonitor && monitor.deviceName == deviceName)
            return monitor.stableId;
    }
    return "GDI:" + deviceName;
}

string MonitorManager::BuildWorkAreaSignature(const MonitorDetail& monitor) {
    const int scalePct = static_cast<int>((monitor.dpiX * 100 + 48) / 96);
    ostringstream ss;
    ss << monitor.width << "x" << monitor.height << "@" << scalePct;
    return ss.str();
}

vector<MonitorDetail> MonitorManager::GetConnectedMonitors() {
    vector<MonitorDetail> monitors;
    if (!EnumDisplayMonitors(nullptr, nullptr, MonitorEnumCallback, reinterpret_cast<LPARAM>(&monitors)))
        return {};

    const auto edidMap = QueryEdidTargetsByMonitor();
    for (auto& mon : monitors) {
        const auto it = edidMap.find(mon.hMonitor);
        if (it != edidMap.end()) {
            if (it->second.valid) mon.stableId = it->second.stableId;
            if (!it->second.friendlyName.empty()) mon.friendlyName = it->second.friendlyName;
        }
        if (mon.stableId.empty()) {
            mon.stableId = "GDI:" + mon.deviceName;
        }
        mon.workAreaSignature = BuildWorkAreaSignature(mon);
    }

    DisambiguateDuplicateStableIds(monitors);
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

bool MonitorManager::GetMonitorByStableId(const string& stableId, MonitorDetail& outMonitor) {
    if (stableId.empty()) return false;
    for (const auto& mon : GetConnectedMonitors()) {
        if (mon.stableId == stableId) {
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

MonitorResolveResult MonitorManager::ResolveMonitorForBox(const MonitorBoxRef& box) {
    return ResolveMonitorForBox(box, GetConnectedMonitors());
}

bool MonitorManager::GetWorkAreaForBox(int monitorIndex,
                                       const string& monitorDevice,
                                       const string& stableMonitorId,
                                       RECT& outWork) {
    MonitorBoxRef ref{};
    ref.monitorIndex = monitorIndex;
    ref.monitorDevice = monitorDevice;
    ref.stableMonitorId = stableMonitorId;

    // Resolve identity and bounds from the same snapshot, avoiding index reuse
    // between two enumerations during a display disconnection.
    const auto monitors = GetConnectedMonitors();
    const MonitorResolveResult resolved = ResolveMonitorForBox(ref, monitors);
    if (resolved.resolvedIndex < 0) return false;

    for (const auto& monitor : monitors) {
        if (monitor.index == resolved.resolvedIndex) {
            outWork = monitor.rcWork;
            return true;
        }
    }
    return false;
}

string MonitorManager::GetCurrentTopologyHash() {
    return GetTopologyHash(GetConnectedMonitors());
}

string MonitorManager::GetTopologyHash(const vector<MonitorDetail>& monitors) {
    vector<string> ids;
    ids.reserve(monitors.size());
    for (const auto& mon : monitors) {
        ids.push_back(mon.stableId);
    }
    sort(ids.begin(), ids.end());

    ostringstream joined;
    for (size_t i = 0; i < ids.size(); ++i) {
        if (i > 0) joined << '|';
        joined << ids[i];
    }

    ostringstream ss;
    ss << hex << setfill('0') << setw(8) << Fnv1aHash(joined.str());
    return ss.str();
}

string MonitorManager::SerializeMonitorsJson() {
    const auto monitors = GetConnectedMonitors();
    const string topologyHash = GetTopologyHash(monitors);

    ostringstream ss;
    ss << "{\"topologyHash\":\"" << topologyHash << "\",\"monitors\":[";
    for (size_t i = 0; i < monitors.size(); ++i) {
        const auto& mon = monitors[i];
        if (i > 0) ss << ',';
        ss << "{"
           << "\"stableId\":\"" << EscapeJson(mon.stableId) << "\","
           << "\"deviceName\":\"" << EscapeJson(mon.deviceName) << "\","
           << "\"friendlyName\":\"" << EscapeJson(mon.friendlyName) << "\","
           << "\"isPrimary\":" << (mon.isPrimary ? "true" : "false") << ","
           << "\"workW\":" << mon.width << ","
           << "\"workH\":" << mon.height << ","
           << "\"workX\":" << mon.rcWork.left << ","
           << "\"workY\":" << mon.rcWork.top << ","
           << "\"dpiX\":" << mon.dpiX << ","
           << "\"dpiY\":" << mon.dpiY << ","
           << "\"scaleX\":" << mon.scaleX << ","
           << "\"scaleY\":" << mon.scaleY << ","
           << "\"signature\":\"" << EscapeJson(mon.workAreaSignature) << "\""
           << "}";
    }
    ss << "]}";
    return ss.str();
}

MonitorManager::BiomeMonitorHealth MonitorManager::EvaluateBiomeMonitorHealth(
    const vector<MonitorBoxRef>& zones,
    const string& savedTopologyHash) {

    BiomeMonitorHealth health{};
    const auto monitors = GetConnectedMonitors();
    health.connectedMonitors = static_cast<int>(monitors.size());

    set<string> requiredStableIds;
    set<string> requiredDevices;
    set<int> requiredIndexes;
    for (const auto& zone : zones) {
        if (!zone.stableMonitorId.empty()) requiredStableIds.insert(zone.stableMonitorId);
        else if (!zone.monitorDevice.empty()) requiredDevices.insert(zone.monitorDevice);
        else requiredIndexes.insert(zone.monitorIndex);
    }
    health.requiredMonitors = static_cast<int>(
        max(requiredStableIds.size(), max(requiredDevices.size(), requiredIndexes.size())));
    if (health.requiredMonitors == 0 && !zones.empty()) {
        health.requiredMonitors = 1;
    }

    const string currentHash = GetTopologyHash(monitors);
    if (!savedTopologyHash.empty() && savedTopologyHash != currentHash) {
        health.topologyMatch = false;
    }

    for (const auto& zone : zones) {
        const MonitorResolveResult resolved = ResolveMonitorForBox(zone, monitors);
        if (resolved.resolvedIndex < 0) {
            ++health.missingZones;
        } else {
            ++health.connectedRequired;
        }
    }

    if (health.requiredMonitors > health.connectedMonitors) {
        ostringstream ss;
        ss << "Designed for " << health.requiredMonitors << " screen"
           << (health.requiredMonitors == 1 ? "" : "s")
           << " — opens connected screens only";
        health.warnings.push_back(ss.str());
    }

    return health;
}
