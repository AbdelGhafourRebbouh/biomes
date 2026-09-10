#pragma once
#include <windows.h>
#include <string>
namespace biomes {
class SingleInstance {
public:
    explicit SingleInstance(const std::wstring& identity = L"biomes");
    ~SingleInstance();
    SingleInstance(const SingleInstance&) = delete;
    SingleInstance& operator=(const SingleInstance&) = delete;
    bool IsPrimary() const { return primary_; }
    bool ActivateExisting(bool silent) const;
    const std::wstring& WindowClass() const { return windowClass_; }
    static constexpr UINT ActivateMessage = WM_APP + 190;
private:
    HANDLE mutex_ = nullptr;
    bool primary_ = false;
    std::wstring windowClass_;
};
}
