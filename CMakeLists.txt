cmake_minimum_required(VERSION 3.20)

project(Biomes LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(Biomes WIN32
    resources/biomes.rc
    src/main.cpp
    src/core/hotkey_manager.cpp
    src/core/json_manager.cpp
    src/core/monitor_manager.cpp
    src/core/window_scaler.cpp
    src/core/app_launcher.cpp
    src/ui/grid_overlay.cpp
    src/ui/webview_window.cpp
    src/ui/launch_panel.cpp
)

target_include_directories(Biomes PRIVATE
    resources
    frontend/icons
    include
    include/external/webview2
)

target_link_libraries(Biomes PRIVATE ole32 user32 shell32 advapi32 gdi32 dwmapi shcore dcomp)

# Keep symbols alongside Release builds for local crash-dump diagnosis.
if(MSVC)
    target_compile_options(Biomes PRIVATE $<$<CONFIG:Release>:/Zi>)
    target_link_options(Biomes PRIVATE $<$<CONFIG:Release>:/DEBUG>)
endif()

option(BIOMES_BUILD_TESTS "Build isolated native stability regression tests" OFF)
if(BIOMES_BUILD_TESTS)
    enable_testing()
    add_executable(BiomesStabilityTests tests/stability_tests.cpp src/core/app_launcher.cpp)
    target_include_directories(BiomesStabilityTests PRIVATE include include/external/webview2)
    target_link_libraries(BiomesStabilityTests PRIVATE ole32 user32 shell32 advapi32 gdi32 dwmapi shcore)
    add_test(NAME stability_regressions COMMAND BiomesStabilityTests)
    set_tests_properties(stability_regressions PROPERTIES TIMEOUT 90)
    add_executable(BiomesPanelPreview WIN32 tests/launch_panel_preview.cpp src/ui/launch_panel.cpp)
    target_include_directories(BiomesPanelPreview PRIVATE include include/external/webview2)
    target_compile_definitions(BiomesPanelPreview PRIVATE BIOMES_PANEL_TESTING)
    target_link_libraries(BiomesPanelPreview PRIVATE ole32 user32 shell32 dcomp shlwapi)
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
