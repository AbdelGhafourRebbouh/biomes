#include "core/app_updater.hpp"
#include "core/app_paths.hpp"
#include "external/nlohmann/json.hpp"
#include "release_version.h"
#include <winhttp.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <chrono>
#include <regex>
#include <stdexcept>

namespace biomes {
namespace {
constexpr size_t MaxInstaller = 128 * 1024 * 1024;
constexpr auto FeedUrl = L"https://github.com/AbdelGhafourRebbouh/biomes/releases/download/beta/update.json";
struct InternetHandle {
    HINTERNET value;
    explicit InternetHandle(HINTERNET h) : value(h) { if (!h) throw std::runtime_error("Update connection failed."); }
    ~InternetHandle() { WinHttpCloseHandle(value); }
};
std::vector<unsigned char> Fetch(const std::wstring& url, size_t limit, const std::shared_ptr<std::atomic_bool>& cancelled) {
    if (cancelled->load()) throw std::runtime_error("Update cancelled.");
    URL_COMPONENTS parts{}; parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = DWORD(-1);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS)
        throw std::runtime_error("Update URL is invalid.");
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    InternetHandle session(WinHttpOpen(L"biomes-updater/1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    WinHttpSetTimeouts(session.value, 5000, 5000, 10000, 10000);
    InternetHandle connection(WinHttpConnect(session.value, host.c_str(), parts.nPort, 0));
    InternetHandle request(WinHttpOpenRequest(connection.value, L"GET", path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    if (!WinHttpSetOption(request.value, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy)) ||
        !WinHttpSendRequest(request.value, L"Cache-Control: no-cache\r\n", DWORD(-1), nullptr, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.value, nullptr)) throw std::runtime_error("Could not contact the update server.");
    DWORD status = 0, length = sizeof(status);
    if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &length, nullptr) || status != 200)
        throw std::runtime_error("The update service is unavailable. Please try again later.");
    std::vector<unsigned char> data;
    unsigned char chunk[16384]; DWORD received = 0;
    const auto deadline = GetTickCount64() + 120000;
    for (;;) {
        if (cancelled->load()) throw std::runtime_error("Update cancelled.");
        if (GetTickCount64() >= deadline) throw std::runtime_error("Update download timed out.");
        if (!WinHttpReadData(request.value, chunk, sizeof(chunk), &received)) throw std::runtime_error("Update download failed.");
        if (!received) break;
        if (data.size() + received > limit) throw std::runtime_error("Update download exceeds its expected size.");
        data.insert(data.end(), chunk, chunk + received);
    }
    return data;
}
std::filesystem::path SaveInstaller(const std::vector<unsigned char>& bytes, unsigned build) {
    // AppPaths validates the isolated root; also validate this staging directory.
    const auto folder = AppPaths::Backups() / "updates";
    std::filesystem::create_directories(folder);
    const DWORD attributes = GetFileAttributesW(folder.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) throw std::runtime_error("Unsafe update staging directory.");
    const auto path = folder / (L"biomes-update-" + std::to_wstring(build) + L"-" + std::to_wstring(GetTickCount64()) + L".exe");
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("Could not create the update installer.");
    DWORD written = 0;
    const bool saved = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
        written == bytes.size() && FlushFileBuffers(file);
    CloseHandle(file);
    if (!saved) { DeleteFileW(path.c_str()); throw std::runtime_error("Could not save the update installer."); }
    return path;
}
}
UpdateRelease AppUpdater::ParseFeed(const std::string& text, unsigned installedBuild) {
    if (text.size() > 16384) throw std::runtime_error("Update metadata is too large.");
    const auto data = nlohmann::json::parse(text);
    if (data.at("schemaVersion") != 1 || data.at("channel") != "beta") throw std::runtime_error("Unsupported update metadata.");
    if (!data.at("build").is_number_unsigned() || !data.at("size").is_number_unsigned())
        throw std::runtime_error("Invalid update metadata numbers.");
    const auto build = data.at("build").get<uint64_t>();
    const auto size = data.at("size").get<uint64_t>();
    if (!build || build > 65535 || !size || size > MaxInstaller) throw std::runtime_error("Invalid update metadata bounds.");
    UpdateRelease release;
    release.build = static_cast<unsigned>(build); release.size = static_cast<size_t>(size);
    release.version = data.at("version").get<std::string>();
    release.url = data.at("url").get<std::string>();
    release.sha256 = data.at("sha256").get<std::string>();
    if (release.version != "1.0.0-beta." + std::to_string(build) ||
        release.url != "https://github.com/AbdelGhafourRebbouh/biomes/releases/download/v" + release.version +
                       "/biomesSetup-v" + release.version + ".exe" ||
        !std::regex_match(release.sha256, std::regex("[0-9a-f]{64}")))
        throw std::runtime_error("Update metadata did not pass validation.");
    return release.build > installedBuild ? release : UpdateRelease{};
}
bool AppUpdater::VerifyPayload(const UpdateRelease& release, const std::vector<unsigned char>& bytes) {
    if (bytes.size() != release.size || bytes.empty() || bytes.size() > MaxInstaller) return false;
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return false;
    unsigned char digest[32]{};
    const auto result = BCryptHash(algorithm, nullptr, 0, const_cast<PUCHAR>(bytes.data()),
        static_cast<ULONG>(bytes.size()), digest, sizeof(digest));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (result < 0) return false;
    std::string hex;
    for (const auto byte : digest) { hex += "0123456789abcdef"[byte >> 4]; hex += "0123456789abcdef"[byte & 15]; }
    return hex == release.sha256;
}
AppUpdater::AppUpdater(std::function<void()> notify, std::function<void()> quit)
    : nextCheck_(GetTickCount64() + 20000), notify_(std::move(notify)), quit_(std::move(quit)) {}
AppUpdater::~AppUpdater() { cancelled_->store(true); }
void AppUpdater::BeginCheck(bool manual) {
    if (job_.valid()) return;
    manual_ = manual; nextCheck_ = GetTickCount64() + 6ULL * 60 * 60 * 1000;
    job_ = std::async(std::launch::async, [cancelled = cancelled_] {
        Result result;
        try {
            const auto bytes = Fetch(FeedUrl, 16384, cancelled);
            result.release = ParseFeed(std::string(bytes.begin(), bytes.end()), BIOMES_BUILD_NUMBER);
        } catch (const std::exception& error) { result.error = error.what(); }
        return result;
    });
}
void AppUpdater::Check() {
    if (job_.valid()) { MessageBoxW(nullptr, L"biomes is checking or downloading an update. Please wait.", L"biomes updates", MB_OK); return; }
    if (Available()) Offer(); else BeginCheck(true);
}
void AppUpdater::Offer() {
    const std::wstring version(available_.version.begin(), available_.version.end());
    const auto prompt = L"biomes " + version + L" is available.\n\nUpdate and restart now? Your active biome will close normally; saved layouts and settings will be kept.";
    if (MessageBoxW(nullptr, prompt.c_str(), L"Update and restart", MB_YESNO | MB_ICONINFORMATION | MB_DEFBUTTON2) != IDYES) return;
    manual_ = true;
    const auto release = available_;
    job_ = std::async(std::launch::async, [release, cancelled = cancelled_] {
        Result result;
        try {
            const auto bytes = Fetch(std::wstring(release.url.begin(), release.url.end()), release.size, cancelled);
            if (cancelled->load()) throw std::runtime_error("Update cancelled.");
            if (!VerifyPayload(release, bytes)) throw std::runtime_error("Update checksum verification failed. Nothing was installed.");
            result.installer = SaveInstaller(bytes, release.build);
        } catch (const std::exception& error) { result.error = error.what(); }
        return result;
    });
}
void AppUpdater::Tick() {
    if (!job_.valid()) { if (GetTickCount64() >= nextCheck_) BeginCheck(false); return; }
    if (job_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    auto result = job_.get();
    if (!result.error.empty()) {
        AppPaths::LogError("Updater: " + result.error);
        if (manual_) MessageBoxW(nullptr, std::wstring(result.error.begin(), result.error.end()).c_str(), L"biomes updates", MB_OK | MB_ICONWARNING);
        return;
    }
    if (!result.installer.empty()) {
        const auto parameters = L"/SILENT /SUPPRESSMSGBOXES /NORESTART /NOCLOSEAPPLICATIONS /BIOMESUPDATE=1 /BIOMESPID=" +
            std::to_wstring(GetCurrentProcessId()) + L" /DIR=\"" + AppPaths::ExecutableDirectory().wstring() + L"\"";
        SHELLEXECUTEINFOW launch{}; launch.cbSize = sizeof(launch); launch.lpVerb = L"open";
        launch.lpFile = result.installer.c_str(); launch.lpParameters = parameters.c_str(); launch.nShow = SW_SHOWNORMAL;
        if (ShellExecuteExW(&launch)) { quit_(); }
        else MessageBoxW(nullptr, L"Could not start the installer. biomes will remain open.", L"biomes updates", MB_OK | MB_ICONERROR);
        return;
    }
    const bool changed = result.release.build != available_.build;
    available_ = result.release;
    if (Available()) { if (manual_) Offer(); else if (changed) notify_(); }
    else if (manual_) MessageBoxW(nullptr, L"You have the latest biomes beta.", L"biomes updates", MB_OK | MB_ICONINFORMATION);
}
}
