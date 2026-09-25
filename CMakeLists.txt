cmake_minimum_required(VERSION 3.20)

project(Biomes VERSION 1.0.0 LANGUAGES CXX RC)
set(BIOMES_BUILD_NUMBER "0" CACHE STRING "Monotonic release build number")
set(BIOMES_RELEASE_VERSION "1.0.0-beta" CACHE STRING "Published release version")
if(NOT BIOMES_BUILD_NUMBER MATCHES "^[0-9]+$" OR BIOMES_BUILD_NUMBER GREATER 65535)
    message(FATAL_ERROR "BIOMES_BUILD_NUMBER must be 0..65535")
endif()
if(NOT BIOMES_RELEASE_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+-beta(\\.[0-9]+)?$")
    message(FATAL_ERROR "Invalid beta version")
endif()
configure_file(resources/release_version.h.in generated/release_version.h @ONLY)

# Apply consistently to the application and regression executables.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
include(CheckIPOSupported)
check_ipo_supported(RESULT BIOMES_IPO_SUPPORTED OUTPUT BIOMES_IPO_ERROR LANGUAGES CXX)
if(BIOMES_IPO_SUPPORTED)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
else()
    message(WARNING "Release IPO unavailable: ${BIOMES_IPO_ERROR}")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(Biomes WIN32
    resources/resource.rc
    src/main.cpp
    src/core/hotkey_manager.cpp
    src/core/ipc_bridge.cpp
    src/core/json_manager.cpp
    src/core/app_paths.cpp
    src/core/legacy_data_migration.cpp
    src/core/background_host.cpp
    src/core/native_settings.cpp
    src/core/single_instance.cpp
    src/core/startup_options.cpp
    src/core/startup_registration.cpp
    src/ui/tray_manager.cpp
    src/core/monitor_manager.cpp
    src/core/window_scaler.cpp
    src/core/app_launcher.cpp
    src/core/app_updater.cpp
    src/ui/grid_overlay.cpp
    src/ui/webview_window.cpp
    src/ui/launch_panel.cpp
)

target_include_directories(Biomes PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}/generated
    resources
    include
    include/external/webview2
)

target_link_libraries(Biomes PRIVATE ole32 user32 shell32 advapi32 gdi32 dwmapi shcore dcomp winhttp bcrypt)

# Keep symbols alongside Release builds for local crash-dump diagnosis.
if(MSVC)
    target_compile_options(Biomes PRIVATE $<$<AND:$<CONFIG:Release>,$<COMPILE_LANGUAGE:CXX>>:/O2;/Zi>)
    # The resource file embeds the authoritative manifest; suppress a second one.
    target_link_options(Biomes PRIVATE /MANIFEST:NO $<$<CONFIG:Release>:/DEBUG;/OPT:REF;/OPT:ICF>)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(Biomes PRIVATE $<$<AND:$<CONFIG:Release>,$<COMPILE_LANGUAGE:CXX>>:-O3>)
endif()

option(BIOMES_BUILD_TESTS "Build isolated native stability regression tests" OFF)
if(BIOMES_BUILD_TESTS)
    enable_testing()
    add_executable(BiomesUpdaterTests tests/updater_tests.cpp src/core/app_updater.cpp src/core/app_paths.cpp)
    target_include_directories(BiomesUpdaterTests PRIVATE include ${CMAKE_CURRENT_BINARY_DIR}/generated)
    target_link_libraries(BiomesUpdaterTests PRIVATE ole32 shell32 user32 winhttp bcrypt)
    add_test(NAME updater_regressions COMMAND BiomesUpdaterTests)
    add_executable(BiomesIpcTests src/core/ipc_bridge_tests.cpp src/core/ipc_bridge.cpp)
    target_include_directories(BiomesIpcTests PRIVATE include)
    add_test(NAME ipc_regressions COMMAND BiomesIpcTests)
    add_executable(BiomesWebViewTests src/ui/webview_window_tests.cpp src/core/app_paths.cpp)
    target_compile_definitions(BiomesWebViewTests PRIVATE BIOMES_STORAGE_TESTING)
    target_include_directories(BiomesWebViewTests PRIVATE include include/external/webview2)
    target_link_libraries(BiomesWebViewTests PRIVATE user32 gdi32 dwmapi ole32 shell32)
    add_test(NAME webview_regressions COMMAND BiomesWebViewTests)
    set_tests_properties(webview_regressions PROPERTIES TIMEOUT 75)
    add_executable(BiomesLayoutTests tests/layout_tests.cpp src/core/json_manager.cpp
        src/core/monitor_manager.cpp src/core/window_scaler.cpp src/core/app_launcher.cpp
        src/core/app_paths.cpp src/ui/grid_overlay.cpp)
    target_compile_definitions(BiomesLayoutTests PRIVATE BIOMES_STORAGE_TESTING)
    target_include_directories(BiomesLayoutTests PRIVATE include include/external/webview2)
    target_link_libraries(BiomesLayoutTests PRIVATE ole32 user32 shell32 advapi32 gdi32 dwmapi shcore)
    add_test(NAME layout_regressions COMMAND BiomesLayoutTests)
    set_tests_properties(layout_regressions PROPERTIES TIMEOUT 30)
    add_executable(BiomesMonitorTests tests/monitor_tests.cpp)
    target_include_directories(BiomesMonitorTests PRIVATE include)
    target_link_libraries(BiomesMonitorTests PRIVATE user32 shcore)
    add_test(NAME monitor_regressions COMMAND BiomesMonitorTests)
    add_executable(BiomesStabilityTests tests/stability_tests.cpp src/core/app_launcher.cpp src/core/app_paths.cpp)
    target_compile_definitions(BiomesStabilityTests PRIVATE BIOMES_STORAGE_TESTING)
    target_include_directories(BiomesStabilityTests PRIVATE include include/external/webview2)
    target_link_libraries(BiomesStabilityTests PRIVATE ole32 user32 shell32 advapi32 gdi32 dwmapi shcore)
    add_test(NAME stability_regressions COMMAND BiomesStabilityTests)
    set_tests_properties(stability_regressions PROPERTIES TIMEOUT 90)
    add_executable(BiomesPanelPreview WIN32 tests/launch_panel_preview.cpp src/ui/launch_panel.cpp src/core/app_paths.cpp)
    target_include_directories(BiomesPanelPreview PRIVATE include include/external/webview2)
    target_compile_definitions(BiomesPanelPreview PRIVATE BIOMES_PANEL_TESTING BIOMES_STORAGE_TESTING)
    target_link_libraries(BiomesPanelPreview PRIVATE ole32 user32 shell32 dcomp shlwapi)
    add_executable(BiomesStorageTests tests/storage_tests.cpp src/core/app_paths.cpp src/core/legacy_data_migration.cpp)
    target_include_directories(BiomesStorageTests PRIVATE include)
    target_compile_definitions(BiomesStorageTests PRIVATE BIOMES_STORAGE_TESTING)
    target_link_libraries(BiomesStorageTests PRIVATE ole32 shell32)
    foreach(scenario paths migrate existing invalid locked once wrongtype)
        add_test(NAME storage_${scenario} COMMAND BiomesStorageTests ${scenario})
    endforeach()
    add_executable(BiomesLifecycleTests tests/lifecycle_tests.cpp
        src/core/app_paths.cpp src/core/background_host.cpp src/core/native_settings.cpp
        src/core/single_instance.cpp src/core/startup_options.cpp src/core/startup_registration.cpp src/ui/tray_manager.cpp)
    target_include_directories(BiomesLifecycleTests PRIVATE include)
    target_compile_definitions(BiomesLifecycleTests PRIVATE BIOMES_STORAGE_TESTING)
    target_link_libraries(BiomesLifecycleTests PRIVATE ole32 shell32 advapi32 user32)
    add_test(NAME lifecycle_regressions COMMAND BiomesLifecycleTests)
    set_tests_properties(lifecycle_regressions PROPERTIES TIMEOUT 30)
endif()

add_custom_target(BiomesAssets ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/WebView2Loader.dll
        $<TARGET_FILE_DIR:Biomes>/WebView2Loader.dll
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/frontend/index.html
        $<TARGET_FILE_DIR:Biomes>/index.html
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/frontend/app.js
        ${CMAKE_SOURCE_DIR}/frontend/save-dialog.js
        ${CMAKE_SOURCE_DIR}/frontend/newsletter.js
        ${CMAKE_SOURCE_DIR}/frontend/styles.css
        ${CMAKE_SOURCE_DIR}/frontend/launch-panel.html
        ${CMAKE_SOURCE_DIR}/frontend/launch-panel.css
        ${CMAKE_SOURCE_DIR}/frontend/launch-panel.js
        ${CMAKE_SOURCE_DIR}/frontend/onboarding.css
        ${CMAKE_SOURCE_DIR}/frontend/onboarding.js
        $<TARGET_FILE_DIR:Biomes>
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/frontend/assets $<TARGET_FILE_DIR:Biomes>/assets
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/frontend/logo $<TARGET_FILE_DIR:Biomes>/logo
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/frontend/images $<TARGET_FILE_DIR:Biomes>/images
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/frontend/cardsimages $<TARGET_FILE_DIR:Biomes>/cardsimages
    DEPENDS Biomes
)
