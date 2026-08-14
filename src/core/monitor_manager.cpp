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
            default:   ss << c; break;
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
    string out(static_cast<size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), needed, nullptr, nullptr);
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

int FindPrimaryIndex(const vector<MonitorDetail>& monitors) {
    for (const auto& mon : monitors) {
        if (mon.isPrimary) return mon.index;
    }
    return monitors.empty() ? -1 : 0;
}

} // namespace

string MonitorManager::BuildStableMonitorId(HMONITOR hMonitor, const string& deviceName) {
    const auto edidMap = QueryEdidTargetsByMonitor();
    const auto it = edidMap.find(hMonitor);
    if (it != edidMap.end() && it->second.valid) {
        return it->second.stableId;
    }
    return "GDI:" + deviceName;
}

string MonitorManager::BuildWorkAreaSignature(const MonitorDetail& monitor) {
    UINT dpiX = 96;
    UINT dpiY = 96;
    GetDpiForMonitor(monitor.hMonitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
    const int scalePct = static_cast<int>((dpiX * 100 + 48) / 96);
    ostringstream ss;
    ss << monitor.width << "x" << monitor.height << "@" << scalePct;
    return ss.str();
}

vector<MonitorDetail> MonitorManager::GetConnectedMonitors() {
    vector<MonitorDetail> monitors;
    EnumDisplayMonitors(nullptr, nullptr, MonitorEnumCallback, reinterpret_cast<LPARAM>(&monitors));

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
    MonitorResolveResult result;
    const auto monitors = GetConnectedMonitors();

    // 1) Hardware identity — if saved, trust it; never fall through to index
    //    (avoids dumping secondary zones onto the only remaining screen).
    if (!box.stableMonitorId.empty()) {
        MonitorDetail detail;
        if (GetMonitorByStableId(box.stableMonitorId, detail)) {
            result.resolvedIndex = detail.index;
            result.matchKind = MonitorMatchKind::StableId;
            result.matchDetail = "stableId=" + box.stableMonitorId;
            cout << "[MONITOR] zone matched via stableId " << box.stableMonitorId
                 << " -> index " << detail.index << endl;
            return result;
        }
        if (!box.monitorDevice.empty()) {
            if (GetMonitorByName(box.monitorDevice, detail)) {
                result.resolvedIndex = detail.index;
                result.matchKind = MonitorMatchKind::DeviceName;
                result.matchDetail = "device=" + box.monitorDevice;
                cout << "[MONITOR] zone matched via deviceName " << box.monitorDevice
                     << " -> index " << detail.index << endl;
                return result;
            }
        }
        result.matchKind = MonitorMatchKind::NotFound;
        result.skipReason = (monitors.size() == 1)
            ? "secondary zone skipped (single monitor)"
            : "monitor disconnected (stableId not found)";
        cout << "[MONITOR] zone skipped: " << result.skipReason
             << " stableId=" << box.stableMonitorId << endl;
        return result;
    }

    // 2) GDI device name (legacy / no EDID)
    if (!box.monitorDevice.empty()) {
        MonitorDetail detail;
        if (GetMonitorByName(box.monitorDevice, detail)) {
            result.resolvedIndex = detail.index;
            result.matchKind = MonitorMatchKind::DeviceName;
            result.matchDetail = "device=" + box.monitorDevice;
            cout << "[MONITOR] zone matched via deviceName " << box.monitorDevice
                 << " -> index " << detail.index << endl;
            return result;
        }
        result.matchKind = MonitorMatchKind::NotFound;
        result.skipReason = (monitors.size() == 1)
            ? "secondary zone skipped (single monitor)"
            : "monitor disconnected";
        cout << "[MONITOR] zone skipped: " << result.skipReason
             << " device=" << box.monitorDevice << endl;
        return result;
    }

    // 3) Index-only legacy: on a single screen, only accept the primary index.
    if (monitors.size() == 1) {
        const int primaryIndex = FindPrimaryIndex(monitors);
        if (box.monitorIndex == primaryIndex) {
            result.resolvedIndex = primaryIndex;
            result.matchKind = MonitorMatchKind::Index;
            result.matchDetail = "index=" + to_string(box.monitorIndex);
            return result;
        }
        result.matchKind = MonitorMatchKind::NotFound;
        result.skipReason = "secondary zone skipped (single monitor)";
        return result;
    }

    if (box.monitorIndex >= 0 && box.monitorIndex < static_cast<int>(monitors.size())) {
        result.resolvedIndex = box.monitorIndex;
        result.matchKind = MonitorMatchKind::Index;
        result.matchDetail = "index=" + to_string(box.monitorIndex);
        cout << "[MONITOR] zone matched via index " << box.monitorIndex << endl;
        return result;
    }

    result.matchKind = MonitorMatchKind::NotFound;
    result.skipReason = "monitor disconnected";
    return result;
}

bool MonitorManager::GetWorkAreaForBox(int monitorIndex,
                                       const string& monitorDevice,
                                       const string& stableMonitorId,
                                       RECT& outWork) {
    MonitorBoxRef ref{};
    ref.monitorIndex = monitorIndex;
    ref.monitorDevice = monitorDevice;
    ref.stableMonitorId = stableMonitorId;

    const MonitorResolveResult resolved = ResolveMonitorForBox(ref);
    if (resolved.resolvedIndex < 0) return false;

    const auto monitors = GetConnectedMonitors();
    if (resolved.resolvedIndex >= static_cast<int>(monitors.size())) return false;

    outWork = monitors[resolved.resolvedIndex].rcWork;
    return true;
}

string MonitorManager::GetCurrentTopologyHash() {
    auto monitors = GetConnectedMonitors();
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
    const string topologyHash = GetCurrentTopologyHash();

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

    const string currentHash = GetCurrentTopologyHash();
    if (!savedTopologyHash.empty() && savedTopologyHash != currentHash) {
        health.topologyMatch = false;
    }

    for (const auto& zone : zones) {
        const MonitorResolveResult resolved = ResolveMonitorForBox(zone);
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
