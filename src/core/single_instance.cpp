#include "core/single_instance.hpp"
#include <sddl.h>
#include <vector>
#include <stdexcept>
namespace biomes {
SingleInstance::SingleInstance(const std::wstring& identity) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) throw std::runtime_error("Cannot identify current user");
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<BYTE> buffer(size);
    const bool okay = GetTokenInformation(token, TokenUser, buffer.data(), size, &size) != FALSE;
    CloseHandle(token);
    if (!okay) throw std::runtime_error("Cannot read user identity");
    LPWSTR sid = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &sid)) throw std::runtime_error("Cannot encode user identity");
    windowClass_ = identity + L".Background." + sid;
    LocalFree(sid);
    mutex_ = CreateMutexW(nullptr, FALSE, (L"Local\\" + windowClass_).c_str());
    const DWORD error = GetLastError();
    if (!mutex_) throw std::runtime_error("Cannot acquire single-instance guard");
    primary_ = error != ERROR_ALREADY_EXISTS;
}
SingleInstance::~SingleInstance() { if (mutex_) CloseHandle(mutex_); }
bool SingleInstance::ActivateExisting(bool silent) const {
    if (silent) return true;
    for (int retry = 0; retry < 100; ++retry) {
        HWND host = FindWindowW(windowClass_.c_str(), nullptr);
        if (host) {
            DWORD pid = 0; GetWindowThreadProcessId(host, &pid);
            AllowSetForegroundWindow(pid);
            DWORD_PTR response = 0;
            if (SendMessageTimeoutW(host, ActivateMessage, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 500, &response)) return response == 1;
            return false;
        }
        Sleep(20);
    }
    return false;
}
}
