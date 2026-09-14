#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <filesystem>

struct SelectedBox;

class AppLauncher {
public:
    static bool IsPackagedAppPath(const std::string& path);
    // Desktop taskbar IDs (for example "Chrome") are not Store activation IDs.
    static bool RequiresPackagedActivation(const SelectedBox& box,
                                          const std::string& resolvedPath = "");

    static bool IsObsidianExe(const std::string& exeOrPath);
    static bool IsChromeExe(const std::string& exeOrPath);
    static bool IsChromeProfilePicker(const std::string& exeOrPath, const std::string& title);
    // Read only Chrome's last-used profile directory, never account credentials.
    static std::string ResolveChromeProfileDirectory(const std::filesystem::path& userDataDirectory = {});
    static std::string DesktopLaunchArguments(const std::string& exeOrPath,
                                              const std::string& chromeProfile = "");

    // Electron / Chromium hosts that can hang if snapped too aggressively.
    static bool IsFragileElectronHost(const std::string& exeOrPath);

    // Returns empty if unavailable (non-packaged window, API failure).
    static std::string GetAumidForWindow(HWND hwnd);

    // Derive PackageFamilyName!AppId guess from a WindowsApps folder path.
    static std::string GuessAumidFromPackagedPath(const std::string& path,
                                                   const std::string& appIdHint = "App");

    static std::string ResolveAumidForBox(const SelectedBox& box);

    // Ordered AUMID candidates (saved, manifest Id, common fallbacks).
    static std::vector<std::string> ResolveAumidCandidates(const SelectedBox& box);

    // Activate a Store/UWP app without showing a modal error dialog.
    static bool LaunchPackagedApp(const std::string& aumid, DWORD& outPid);

    // Try AUMID candidates until one activates.
    static bool LaunchPackagedAppForBox(const SelectedBox& box, DWORD& outPid);

    // Resolve Obsidian install path (%LocalAppData%\Programs\Obsidian\Obsidian.exe).
    static std::string ResolveObsidianExePath();

    // Launch Obsidian with obsidian:// URI (never bare exe).
    static bool LaunchObsidianWithUri(const std::string& launchUri, DWORD& outPid);

    // Parse Obsidian window title → obsidian://open?vault=...
    static std::string BuildObsidianLaunchUri(const std::string& windowTitle);

    // Prefer saved URI if valid; otherwise rebuild from titleHint + obsidian.json.
    static std::string ResolveObsidianLaunchUri(const SelectedBox& box);

    static std::string UrlEncode(const std::string& value);
};
