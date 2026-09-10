#include "core/app_paths.hpp"
#include "core/startup_options.hpp"
#include "core/single_instance.hpp"
#include "core/background_host.hpp"
#include "core/native_settings.hpp"
#include <fstream>
#include <iostream>
#include <thread>
#include <stdexcept>
namespace fs = std::filesystem;
using json = nlohmann::json;
int checks = 0;
void Require(bool value, const char* message) { ++checks; if (!value) throw std::runtime_error(message); }
template<class F> void Throws(F action) { bool failed = false; try { action(); } catch (...) { failed = true; } Require(failed,"Expected failure"); }
int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--duplicate") {
        const std::string name(argv[2]);
        biomes::SingleInstance child(std::wstring(name.begin(),name.end()));
        return child.IsPrimary() ? 3 : child.ActivateExisting(false) ? 0 : 4;
    }
    const auto id = std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    const auto temp = fs::temp_directory_path() / (L"biomes-lifecycle-" + id);
    const auto key = L"Software\\biomes-tests\\" + id;
    try {
        biomes::AppPaths::InitializeForTests(temp);
        biomes::StartupRegistration registration(key);
        biomes::NativeSettings settings(registration);
        Require(!biomes::StartupOptions::Parse(L"Biomes.exe").silent,"normal launch");
        Require(biomes::StartupOptions::Parse(L"\"C:\\Program Files\\Biomes.exe\" --minimized").silent,"minimized flag");
        Require(biomes::StartupOptions::Parse(L"Biomes.exe --autostart").silent,"autostart flag");
        Throws([] { biomes::StartupOptions::Parse(L"Biomes.exe --unknown"); });
        Require(biomes::StartupRegistration::Command(L"C:\\Program Files\\Biomes.exe") == L"\"C:\\Program Files\\Biomes.exe\" --autostart","quoted startup path");
        Require(settings.Read()["launchAtStartup"] == false,"defaults");
        Require(fs::exists(biomes::AppPaths::SettingsFile()),"settings persisted");
        settings.Update({{"launchAtStartup",true},{"onboardingCompleted",true}});
        Require(registration.IsEnabled(),"startup registered in isolated key");
        Require(settings.Read()["onboardingCompleted"] == true,"onboarding backend state");
        Throws([&] { settings.Update({{"launchAtStartup","yes"}}); });
        Throws([&] { settings.Update({{"registryPath","unsafe"}}); });
        Require(registration.IsEnabled(),"invalid patch leaves registry unchanged");
        // Unrelated registry values must survive disabling.
        RegSetKeyValueW(HKEY_CURRENT_USER,key.c_str(),L"other",REG_DWORD,&checks,sizeof(checks));
        settings.Update({{"launchAtStartup",false}});
        Require(!registration.Read(),"startup removed");
        DWORD bytes=sizeof(DWORD), other=0;
        Require(RegGetValueW(HKEY_CURRENT_USER,key.c_str(),L"other",RRF_RT_REG_DWORD,nullptr,&other,&bytes)==ERROR_SUCCESS,"unrelated value retained");
        auto data=settings.Read(); data["futureField"]="keep";
        { std::ofstream f(biomes::AppPaths::SettingsFile()); f << data; }
        settings.Update({{"onboardingCompleted",false}});
        Require(settings.Read()["futureField"]=="keep","unknown fields retained");
        // A failed settings save must roll back the registry change.
        fs::create_directory(biomes::AppPaths::Config() / L"settings.json.tmp");
        Throws([&] { settings.Update({{"launchAtStartup",true}}); });
        Require(!registration.Read(),"registry rollback");
        fs::remove(biomes::AppPaths::Config() / L"settings.json.tmp");
        data=settings.Read(); data["schemaVersion"]=999;
        { std::ofstream f(biomes::AppPaths::SettingsFile()); f << data; }
        Throws([&] { settings.Read(); });
        Throws([&] { settings.Update({{"onboardingCompleted",true}}); });
        { std::ifstream f(biomes::AppPaths::SettingsFile()); Require(json::parse(f)["schemaVersion"]==999,"future schema never overwritten"); }
        const auto identity=L"biomes-test-"+id;
        biomes::SingleInstance primary(identity), duplicate(identity);
        Require(primary.IsPrimary() && !duplicate.IsPrimary(),"single-instance guard");
        biomes::BackgroundHost host;
        Require(host.Initialize(GetModuleHandleW(nullptr),primary.WindowClass(),false),"hidden host created");
        Require(!IsWindowVisible(host.Hwnd()),"no UI flash");
        int opened=0, hotkeys=0, stopped=0;
        host.open=[&] { ++opened; PostMessageW(host.Hwnd(),WM_HOTKEY,77,0); };
        host.hotkey=[&](int id) { Require(id==77,"host hotkey id"); ++hotkeys; host.RequestExit(); };
        host.shutdown=[&] { ++stopped; };
        bool handedOff=false;
        std::thread sender([&] {
            std::wstring command=L"\""+(biomes::AppPaths::ExecutableDirectory()/L"BiomesLifecycleTests.exe").wstring()+L"\" --duplicate "+identity;
            STARTUPINFOW si{sizeof(si)}; PROCESS_INFORMATION pi{};
            if (CreateProcessW(nullptr,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi)) {
                CloseHandle(pi.hThread);
                DWORD code=1;
                if (WaitForSingleObject(pi.hProcess,10000)==WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess,&code);
                handedOff=code==0; CloseHandle(pi.hProcess);
            }
            if (!handedOff) host.RequestExit();
        });
        Require(host.Run()==0,"message loop exited");
        sender.join();
        Require(handedOff && opened==1 && hotkeys==1 && stopped==1,"handoff hotkeys and shutdown");
        Require(!host.Hwnd(),"host destroyed on exit");
        Require(duplicate.ActivateExisting(true),"duplicate autostart exits silently");
        RegDeleteTreeW(HKEY_CURRENT_USER,key.c_str());
        fs::remove_all(temp); // Only this test's unique temporary fixture.
        std::cout << "PASS " << checks << " lifecycle checks; no real startup entry or user data used\n";
        return 0;
    } catch(const std::exception& e) {
        std::cerr << e.what() << " (isolated fixtures retained)\n";
        RegDeleteTreeW(HKEY_CURRENT_USER,key.c_str());
        return 1;
    }
}
