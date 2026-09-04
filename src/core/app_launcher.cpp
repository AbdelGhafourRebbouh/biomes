#include "../../include/core/app_launcher.hpp"
#include "../../include/ui/grid_overlay.hpp"
#include "../../include/external/nlohmann/json.hpp"

#include <vector>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <iterator>
#include <filesystem>
#include <algorithm>
#include <cctype>

#include <windows.h>
#include <appmodel.h>
#include <shobjidl.h>
#include <shellapi.h>

#pragma comment(lib, "ole32.lib")

using namespace std;
using json = nlohmann::json;

namespace {

struct ObsidianVaultInfo {
    string id;
    string path;
    string name; // folder basename — what Obsidian uses as vault name
    bool open = false;
    long long ts = 0;
};

string ToLower(string value) {
    transform(value.begin(), value.end(), value.begin(),
              [](unsigned char c) { return static_cast<char>(tolower(c)); });
    return value;
}

string WideToUtf8(const wstring& wide) {
    if (wide.empty()) return "";
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) return "";
    // The reported size includes the trailing null. Allocate room for it before
    // calling the Win32 API, then remove it from the C++ string representation.
    string out(static_cast<size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, out.data(), size, nullptr, nullptr) <= 0) {
        return "";
    }
    out.pop_back();
    return out;
}

wstring Utf8ToWide(const string& utf8) {
    if (utf8.empty()) return L"";
    const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (size <= 0) return L"";
    // As above, reserve the null terminator required by MultiByteToWideChar.
    wstring out(static_cast<size_t>(size), L'\0');
    if (MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, out.data(), size) <= 0) {
        return L"";
    }
    out.pop_back();
    return out;
}

string ExpandEnv(const string& value) {
    char buffer[MAX_PATH * 4];
    const DWORD written = ExpandEnvironmentStringsA(value.c_str(), buffer, static_cast<DWORD>(sizeof(buffer)));
    if (written == 0 || written > sizeof(buffer)) return value;
    return string(buffer);
}

bool FileExists(const string& path) {
    const DWORD attrs = GetFileAttributesA(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

bool DirectoryExists(const string& path) {
    const DWORD attrs = GetFileAttributesA(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

string ExeBaseName(const string& path) {
    return filesystem::path(path).filename().string();
}

string TrimCopy(string value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(value.begin());
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.pop_back();
    return value;
}

bool EqualsIgnoreCase(const string& a, const string& b) {
    return _stricmp(a.c_str(), b.c_str()) == 0;
}

bool ContainsIgnoreCase(const string& haystack, const string& needle) {
    if (needle.empty()) return false;
    return ToLower(haystack).find(ToLower(needle)) != string::npos;
}

// "Obsidian" or "Obsidian 1.13.6" — never a real vault name.
bool LooksLikeObsidianAppLabel(const string& value) {
    const string lower = ToLower(TrimCopy(value));
    if (lower == "obsidian") return true;
    if (lower.rfind("obsidian ", 0) == 0) {
        const string rest = TrimCopy(lower.substr(9));
        if (rest.empty()) return true;
        return all_of(rest.begin(), rest.end(), [](unsigned char c) {
            return isdigit(c) || c == '.';
        });
    }
    return false;
}

// Titles look like: "note - Vault Name - Obsidian 1.13.6" or "Vault Name - Obsidian"
string StripObsidianAppSuffix(string title) {
    title = TrimCopy(title);
    const string lower = ToLower(title);
    const string marker = " - obsidian";
    const size_t pos = lower.rfind(marker);
    if (pos == string::npos) return title;

    const string after = lower.substr(pos + marker.size()); // "" or " 1.13.6"
    if (after.empty() || after[0] == ' ') {
        const string rest = TrimCopy(after);
        if (rest.empty() || all_of(rest.begin(), rest.end(), [](unsigned char c) {
                return isdigit(c) || c == '.';
            })) {
            return TrimCopy(title.substr(0, pos));
        }
    }
    return title;
}

string ExtractVaultCandidateFromTitle(const string& windowTitle) {
    string core = StripObsidianAppSuffix(windowTitle);
    if (core.empty()) return "";

    const size_t dash = core.rfind(" - ");
    if (dash != string::npos && dash + 3 < core.size()) {
        return TrimCopy(core.substr(dash + 3));
    }
    return TrimCopy(core);
}

vector<ObsidianVaultInfo> LoadObsidianVaults() {
    vector<ObsidianVaultInfo> vaults;
    const string path = ExpandEnv("%APPDATA%\\obsidian\\obsidian.json");
    ifstream in(path);
    if (!in) return vaults;

    json root;
    try {
        in >> root;
    } catch (...) {
        return vaults;
    }

    if (!root.contains("vaults") || !root["vaults"].is_object()) return vaults;

    for (auto it = root["vaults"].begin(); it != root["vaults"].end(); ++it) {
        ObsidianVaultInfo info;
        info.id = it.key();
        const json& entry = it.value();
        if (!entry.is_object()) continue;
        info.path = entry.value("path", "");
        info.open = entry.value("open", false);
        info.ts = entry.value("ts", 0LL);
        if (!info.path.empty()) {
            try {
                info.name = filesystem::path(info.path).filename().string();
            } catch (...) {
                info.name.clear();
            }
        }
        if (!info.id.empty()) vaults.push_back(std::move(info));
    }

    sort(vaults.begin(), vaults.end(), [](const ObsidianVaultInfo& a, const ObsidianVaultInfo& b) {
        return a.ts > b.ts;
    });
    return vaults;
}

string UriFromVault(const ObsidianVaultInfo& vault) {
    // Vault ID is the most stable key (survives renames better than display quirks).
    if (!vault.id.empty()) {
        return "obsidian://open?vault=" + vault.id;
    }
    if (!vault.name.empty()) {
        return "obsidian://open?vault=" + AppLauncher::UrlEncode(vault.name);
    }
    return "";
}

string ExtractVaultParam(const string& uri) {
    const string key = "vault=";
    const size_t pos = ToLower(uri).find(key);
    if (pos == string::npos) return "";
    size_t start = pos + key.size();
    size_t end = uri.find('&', start);
    string raw = (end == string::npos) ? uri.substr(start) : uri.substr(start, end - start);

    // Minimal URL decode for spaces / common cases
    string out;
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '%' && i + 2 < raw.size()) {
            const string hex = raw.substr(i + 1, 2);
            char* endp = nullptr;
            const long value = strtol(hex.c_str(), &endp, 16);
            if (endp && *endp == '\0') {
                out.push_back(static_cast<char>(value));
                i += 2;
                continue;
            }
        } else if (raw[i] == '+') {
            out.push_back(' ');
            continue;
        }
        out.push_back(raw[i]);
    }
    return TrimCopy(out);
}

bool VaultParamIsKnown(const string& vaultParam, const vector<ObsidianVaultInfo>& vaults) {
    if (vaultParam.empty() || LooksLikeObsidianAppLabel(vaultParam)) return false;
    for (const auto& v : vaults) {
        if (EqualsIgnoreCase(vaultParam, v.id) || EqualsIgnoreCase(vaultParam, v.name)) {
            return true;
        }
    }
    return false;
}

string MatchVaultUriFromTitle(const string& windowTitle, const vector<ObsidianVaultInfo>& vaults) {
    if (vaults.empty()) {
        const string candidate = ExtractVaultCandidateFromTitle(windowTitle);
        if (!candidate.empty() && !LooksLikeObsidianAppLabel(candidate)) {
            return "obsidian://open?vault=" + AppLauncher::UrlEncode(candidate);
        }
        return "";
    }

    // Prefer registered vault whose folder name appears in the title (longest match wins).
    const ObsidianVaultInfo* best = nullptr;
    size_t bestLen = 0;
    for (const auto& v : vaults) {
        if (v.name.empty()) continue;
        if (ContainsIgnoreCase(windowTitle, v.name) && v.name.size() > bestLen) {
            best = &v;
            bestLen = v.name.size();
        }
    }
    if (best) return UriFromVault(*best);

    const string candidate = ExtractVaultCandidateFromTitle(windowTitle);
    if (!candidate.empty() && !LooksLikeObsidianAppLabel(candidate)) {
        for (const auto& v : vaults) {
            if (EqualsIgnoreCase(candidate, v.name) || EqualsIgnoreCase(candidate, v.id)) {
                return UriFromVault(v);
            }
        }
        return "obsidian://open?vault=" + AppLauncher::UrlEncode(candidate);
    }

    // Fallbacks: currently open vault, then most recently used.
    for (const auto& v : vaults) {
        if (v.open && DirectoryExists(v.path)) return UriFromVault(v);
    }
    for (const auto& v : vaults) {
        if (DirectoryExists(v.path)) return UriFromVault(v);
    }
    if (!vaults.empty()) return UriFromVault(vaults.front());
    return "";
}

string FormatHresult(HRESULT value) {
    ostringstream out;
    out << "0x" << uppercase << hex << setw(8) << setfill('0')
        << static_cast<unsigned long>(value);
    return out.str();
}

bool IsValidAumid(const string& aumid) {
    const size_t separator = aumid.find('!');
    return separator != string::npos && separator > 0 &&
           separator + 1 < aumid.size() &&
           aumid.find('!', separator + 1) == string::npos;
}

bool GetPackageIdentityFromPath(const string& path, string& outPackageFamilyName,
                                string& outPackageRoot) {
    outPackageFamilyName.clear();
    outPackageRoot.clear();

    const string lower = ToLower(path);
    const string marker = "\\windowsapps\\";
    const size_t markerPos = lower.find(marker);
    if (markerPos == string::npos) return false;

    const size_t folderStart = markerPos + marker.size();
    const size_t folderEnd = path.find('\\', folderStart);
    const string folder = (folderEnd == string::npos)
        ? path.substr(folderStart)
        : path.substr(folderStart, folderEnd - folderStart);
    const size_t hashSep = folder.rfind("__");
    if (hashSep == string::npos || hashSep + 2 >= folder.size()) return false;

    string packageWithoutPublisher = folder.substr(0, hashSep);
    const size_t architectureSep = packageWithoutPublisher.rfind('_');
    if (architectureSep == string::npos) return false;
    packageWithoutPublisher.erase(architectureSep);

    const size_t versionSep = packageWithoutPublisher.rfind('_');
    if (versionSep == string::npos) return false;

    outPackageFamilyName = packageWithoutPublisher.substr(0, versionSep) + "_" +
                           folder.substr(hashSep + 2);
    outPackageRoot = path.substr(0, folderStart) + folder;
    return true;
}

// A package may declare several launchable applications. Return every declared
// Id so AUMID resolution is not limited to the first manifest entry.
vector<string> ReadApplicationIdsFromManifest(const string& packageRoot) {
    vector<string> ids;
    const string manifestPath = packageRoot + "\\AppxManifest.xml";
    ifstream in(manifestPath, ios::binary);
    if (!in) return ids;

    string xml((istreambuf_iterator<char>(in)), istreambuf_iterator<char>());
    const string lower = ToLower(xml);
    size_t searchFrom = 0;
    while (true) {
        const size_t appTag = lower.find("<application", searchFrom);
        if (appTag == string::npos) break;

        const size_t tagNameEnd = appTag + 12;
        // Do not treat <Applications> as an <Application> element.
        if (tagNameEnd < lower.size() && isalnum(static_cast<unsigned char>(lower[tagNameEnd]))) {
            searchFrom = tagNameEnd;
            continue;
        }

        const size_t tagEnd = xml.find('>', appTag);
        if (tagEnd == string::npos) break;

        const string attrs = xml.substr(appTag, tagEnd - appTag);
        const string attrsLower = ToLower(attrs);
        const size_t idKey = attrsLower.find("id=\"");
        if (idKey != string::npos) {
            const size_t idStart = idKey + 4;
            const size_t idEnd = attrs.find('"', idStart);
            if (idEnd != string::npos && idEnd > idStart) {
                const string id = attrs.substr(idStart, idEnd - idStart);
                bool duplicate = false;
                for (const auto& existing : ids) {
                    if (EqualsIgnoreCase(existing, id)) {
                        duplicate = true;
                        break;
                    }
                }
                if (!duplicate) ids.push_back(id);
            }
        }
        searchFrom = tagEnd + 1;
    }
    return ids;
}

HRESULT ActivatePackagedAppWithCom(const string& aumid, DWORD& outPid) {
    outPid = 0;
    if (!IsValidAumid(aumid)) return E_INVALIDARG;

    const HRESULT initializeHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool mustUninitialize = SUCCEEDED(initializeHr);
    if (FAILED(initializeHr) && initializeHr != RPC_E_CHANGED_MODE) {
        return initializeHr;
    }

    IApplicationActivationManager* activation = nullptr;
    const HRESULT createHr = CoCreateInstance(
        CLSID_ApplicationActivationManager,
        nullptr,
        CLSCTX_LOCAL_SERVER,
        IID_PPV_ARGS(&activation));
    if (FAILED(createHr) || !activation) {
        if (mustUninitialize) CoUninitialize();
        return FAILED(createHr) ? createHr : E_FAIL;
    }

    const wstring wideAumid = Utf8ToWide(aumid);
    const HRESULT activateHr = wideAumid.empty()
        ? E_INVALIDARG
        : activation->ActivateApplication(wideAumid.c_str(), nullptr, AO_NOERRORUI, &outPid);
    activation->Release();
    if (mustUninitialize) CoUninitialize();
    return activateHr;
}

bool ActivatePackagedAppViaAppsFolder(const string& aumid, DWORD& outPid) {
    outPid = 0;
    if (!IsValidAumid(aumid)) {
        cerr << "[LAUNCHER] AppsFolder skipped invalid AUMID: " << aumid << endl;
        return false;
    }

    const wstring target = L"shell:AppsFolder\\" + Utf8ToWide(aumid);
    if (target.empty()) {
        cerr << "[LAUNCHER] AppsFolder conversion failed for " << aumid << endl;
        return false;
    }

    SHELLEXECUTEINFOW executeInfo{};
    executeInfo.cbSize = sizeof(executeInfo);
    executeInfo.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOCLOSEPROCESS;
    executeInfo.lpVerb = L"open";
    executeInfo.lpFile = target.c_str();
    executeInfo.nShow = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&executeInfo)) {
        cerr << "[LAUNCHER] AppsFolder failed for " << aumid
             << " (Win32=" << GetLastError() << ")" << endl;
        return false;
    }

    if (executeInfo.hProcess) {
        outPid = GetProcessId(executeInfo.hProcess);
        CloseHandle(executeInfo.hProcess);
    }
    cout << "[LAUNCHER] AppsFolder activated " << aumid
         << " pid=" << outPid << endl;
    return true;
}

} // namespace

bool AppLauncher::IsPackagedAppPath(const string& path) {
    const string lower = ToLower(path);
    return lower.find("\\windowsapps\\") != string::npos;
}

bool AppLauncher::IsObsidianExe(const string& exeOrPath) {
    const string name = ExeBaseName(exeOrPath);
    return _stricmp(name.c_str(), "Obsidian.exe") == 0;
}

bool AppLauncher::IsFragileElectronHost(const string& exeOrPath) {
    const string name = ToLower(ExeBaseName(exeOrPath));
    static const char* kFragile[] = {
        "notion.exe", "obsidian.exe", "code.exe", "cursor.exe",
        "discord.exe", "slack.exe", "spotify.exe", "ms-teams.exe",
        "teams.exe", "figma.exe", "whatsapp.exe", "localsend_app.exe"
    };
    for (const char* fragile : kFragile) {
        if (name == fragile) return true;
    }
    return false;
}


string AppLauncher::GetAumidForWindow(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return "";

    UINT32 length = 0;
    const LONG rc = GetApplicationUserModelId(hwnd, &length, nullptr);
    if (rc != ERROR_INSUFFICIENT_BUFFER || length == 0) return "";

    vector<wchar_t> buffer(length);
    if (GetApplicationUserModelId(hwnd, &length, buffer.data()) != 0) return "";

    return WideToUtf8(buffer.data());
}

string AppLauncher::GuessAumidFromPackagedPath(const string& path, const string& appIdHint) {
    const string lower = ToLower(path);
    const string marker = "\\windowsapps\\";
    const size_t pos = lower.find(marker);
    if (pos == string::npos) return "";

    const size_t start = pos + marker.size();
    const size_t end = path.find('\\', start);
    const string folder = (end == string::npos) ? path.substr(start) : path.substr(start, end - start);
    const string packageRoot = path.substr(0, start) + folder;

    const size_t hashSep = folder.rfind("__");
    if (hashSep == string::npos || hashSep + 2 >= folder.size()) return "";

    const string publisherHash = folder.substr(hashSep + 2);
    string left = folder.substr(0, hashSep);

    const size_t archSep = left.rfind('_');
    if (archSep == string::npos) return "";
    left = left.substr(0, archSep);

    const size_t verSep = left.rfind('_');
    if (verSep == string::npos) return "";

    const string packageName = left.substr(0, verSep);
    const string pfn = packageName + "_" + publisherHash;

    // Affinity Store uses Id="Canva.Affinity", not "Affinity" — prefer manifest.
    const auto manifestIds = ReadApplicationIdsFromManifest(packageRoot);
    string appId = manifestIds.empty() ? "" : manifestIds.front();
    if (appId.empty()) {
        appId = appIdHint;
        if (_stricmp(appId.c_str(), "App") == 0 || appId.empty()) {
            const string exe = ExeBaseName(path);
            if (_stricmp(exe.c_str(), "Spotify.exe") == 0) appId = "Spotify";
            else {
                appId = exe;
                const size_t dot = appId.rfind('.');
                if (dot != string::npos) appId = appId.substr(0, dot);
            }
        }
    }

    return pfn + "!" + appId;
}

string AppLauncher::ResolveAumidForBox(const SelectedBox& box) {
    const auto candidates = ResolveAumidCandidates(box);
    return candidates.empty() ? "" : candidates.front();
}

vector<string> AppLauncher::ResolveAumidCandidates(const SelectedBox& box) {
    vector<string> out;
    auto addUnique = [&](const string& value) {
        if (value.empty()) return;
        if (!IsValidAumid(value)) {
            cerr << "[LAUNCHER] Ignoring invalid AUMID candidate: " << value << endl;
            return;
        }
        for (const auto& existing : out) {
            if (_stricmp(existing.c_str(), value.c_str()) == 0) return;
        }
        out.push_back(value);
    };

    addUnique(box.aumid);

    if (!box.assignedApp.empty() && IsPackagedAppPath(box.assignedApp)) {
        const string lower = ToLower(box.assignedApp);
        const string marker = "\\windowsapps\\";
        const size_t pos = lower.find(marker);
        if (pos != string::npos) {
            const size_t start = pos + marker.size();
            const size_t end = box.assignedApp.find('\\', start);
            const string folder = (end == string::npos)
                ? box.assignedApp.substr(start)
                : box.assignedApp.substr(start, end - start);
            const string packageRoot = box.assignedApp.substr(0, start) + folder;

            const size_t hashSep = folder.rfind("__");
            if (hashSep != string::npos && hashSep + 2 < folder.size()) {
                const string publisherHash = folder.substr(hashSep + 2);
                string left = folder.substr(0, hashSep);
                const size_t archSep = left.rfind('_');
                if (archSep != string::npos) {
                    left = left.substr(0, archSep);
                    const size_t verSep = left.rfind('_');
                    if (verSep != string::npos) {
                        const string pfn = left.substr(0, verSep) + "_" + publisherHash;
                        for (const auto& manifestId : ReadApplicationIdsFromManifest(packageRoot)) {
                            addUnique(pfn + "!" + manifestId);
                        }

                        string exeStem = box.exeName.empty()
                            ? ExeBaseName(box.assignedApp)
                            : box.exeName;
                        const size_t dot = exeStem.rfind('.');
                        if (dot != string::npos) exeStem = exeStem.substr(0, dot);
                        addUnique(pfn + "!" + exeStem);
                        addUnique(pfn + "!App");
                    }
                }
            }
        }
    }

    return out;
}

bool AppLauncher::LaunchPackagedApp(const string& aumid, DWORD& outPid) {
    outPid = 0;
    if (!IsValidAumid(aumid)) {
        cerr << "[LAUNCHER] COM activation skipped invalid AUMID: " << aumid << endl;
        return false;
    }

    cout << "[LAUNCHER] COM activation candidate: " << aumid << endl;
    const HRESULT activateHr = ActivatePackagedAppWithCom(aumid, outPid);

    if (FAILED(activateHr)) {
        cerr << "[LAUNCHER] ActivateApplication failed for " << aumid
             << " (HRESULT=" << FormatHresult(activateHr) << ")" << endl;
        return false;
    }

    cout << "[LAUNCHER] COM activated packaged app " << aumid << " pid=" << outPid << endl;
    return true;
}

bool AppLauncher::LaunchPackagedAppForBox(const SelectedBox& box, DWORD& outPid) {
    outPid = 0;
    const auto candidates = ResolveAumidCandidates(box);
    if (candidates.empty()) {
        cerr << "[LAUNCHER] No AUMID candidates for packaged app" << endl;
        return false;
    }

    for (const auto& aumid : candidates) {
        cout << "[LAUNCHER] Trying packaged AUMID " << aumid << endl;
        if (LaunchPackagedApp(aumid, outPid)) {
            return true;
        }
    }

    cerr << "[LAUNCHER] All COM AUMID candidates failed; trying AppsFolder activation." << endl;
    for (const auto& aumid : candidates) {
        cout << "[LAUNCHER] AppsFolder activation candidate: " << aumid << endl;
        if (ActivatePackagedAppViaAppsFolder(aumid, outPid)) {
            return true;
        }
    }

    cerr << "[LAUNCHER] All packaged launch routes failed after "
         << candidates.size() << " AUMID candidate(s)." << endl;
    return false;
}

string AppLauncher::ResolveObsidianExePath() {
    const string candidate = ExpandEnv("%LocalAppData%\\Programs\\Obsidian\\Obsidian.exe");
    if (FileExists(candidate)) return candidate;

    const string resolved = ExpandEnv("%LocalAppData%\\Obsidian\\Obsidian.exe");
    if (FileExists(resolved)) return resolved;

    return "";
}

bool AppLauncher::LaunchObsidianWithUri(const string& launchUri, DWORD& outPid) {
    outPid = 0;
    if (launchUri.empty()) {
        cerr << "[LAUNCHER] Obsidian launch requires launchUri" << endl;
        return false;
    }

    string exePath = ResolveObsidianExePath();
    if (exePath.empty() && !launchUri.empty()) {
        exePath = ResolveObsidianExePath();
    }

    if (exePath.empty()) {
        cerr << "[LAUNCHER] Obsidian.exe not found under LocalAppData\\Programs" << endl;
        return false;
    }

    string commandLine = "\"" + exePath + "\" \"" + launchUri + "\"";
    vector<char> cmdBuf(commandLine.begin(), commandLine.end());
    cmdBuf.push_back('\0');

    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};

    const BOOL created = CreateProcessA(
        exePath.c_str(),
        cmdBuf.data(),
        nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi
    );

    if (!created) {
        cerr << "[LAUNCHER] Obsidian URI launch failed (" << GetLastError() << ")" << endl;
        return false;
    }

    outPid = pi.dwProcessId;
    WaitForInputIdle(pi.hProcess, 2000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    cout << "[LAUNCHER] Obsidian URI launch pid=" << outPid << " uri=" << launchUri << endl;
    return true;
}

string AppLauncher::UrlEncode(const string& value) {
    ostringstream encoded;
    encoded << hex << uppercase;
    for (unsigned char c : value) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded << c;
        } else if (c == ' ') {
            encoded << "%20";
        } else {
            encoded << '%' << setw(2) << setfill('0') << static_cast<int>(c);
        }
    }
    return encoded.str();
}

string AppLauncher::BuildObsidianLaunchUri(const string& windowTitle) {
    if (windowTitle.empty()) return "";
    const auto vaults = LoadObsidianVaults();
    return MatchVaultUriFromTitle(windowTitle, vaults);
}

string AppLauncher::ResolveObsidianLaunchUri(const SelectedBox& box) {
    const auto vaults = LoadObsidianVaults();

    if (!box.launchUri.empty()) {
        const string param = ExtractVaultParam(box.launchUri);
        if (VaultParamIsKnown(param, vaults)) {
            return box.launchUri;
        }
        // Old buggy saves used "Obsidian 1.13.6" as the vault — ignore and rebuild.
        if (!LooksLikeObsidianAppLabel(param) && !param.empty() && vaults.empty()) {
            return box.launchUri;
        }
        cout << "[LAUNCHER] Ignoring invalid Obsidian launchUri: " << box.launchUri << endl;
    }

    if (!box.titleHint.empty()) {
        const string built = MatchVaultUriFromTitle(box.titleHint, vaults);
        if (!built.empty()) return built;
    }

    // Last resort: open vault / most recent from Obsidian config.
    for (const auto& v : vaults) {
        if (v.open && DirectoryExists(v.path)) return UriFromVault(v);
    }
    for (const auto& v : vaults) {
        if (DirectoryExists(v.path)) return UriFromVault(v);
    }

    return "";
}
