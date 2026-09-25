#pragma once
#include <windows.h>
#include <filesystem>
#include <functional>
#include <future>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace biomes {
struct UpdateRelease {
    unsigned build = 0;
    std::string version, url, sha256;
    size_t size = 0;
};
// UI methods run on the background host thread. Workers never touch HWNDs.
class AppUpdater {
public:
    AppUpdater(std::function<void()> notify, std::function<void()> quit);
    ~AppUpdater();
    void Tick();
    void Check();
    bool Available() const { return available_.build != 0; }
    static UpdateRelease ParseFeed(const std::string& text, unsigned installedBuild);
    static bool VerifyPayload(const UpdateRelease&, const std::vector<unsigned char>&);
private:
    struct Result { UpdateRelease release; std::filesystem::path installer; std::string error; };
    std::future<Result> job_;
    std::shared_ptr<std::atomic_bool> cancelled_ = std::make_shared<std::atomic_bool>(false);
    UpdateRelease available_;
    bool manual_ = false;
    ULONGLONG nextCheck_ = 0;
    std::function<void()> notify_, quit_;
    void BeginCheck(bool manual);
    void Offer();
};
}
