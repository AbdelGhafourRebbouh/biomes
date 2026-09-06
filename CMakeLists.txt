cmake_minimum_required(VERSION 3.20)

project(Biomes LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(Biomes WIN32
    src/main.cpp
    src/core/hotkey_manager.cpp
    src/core/json_manager.cpp
    src/core/monitor_manager.cpp
    src/core/window_scaler.cpp
    src/core/app_launcher.cpp
    src/ui/grid_overlay.cpp
    src/ui/webview_window.cpp
)

target_include_directories(Biomes PRIVATE
    include
    include/external/webview2
)

target_link_libraries(Biomes PRIVATE ole32 user32 shell32 advapi32 gdi32 dwmapi shcore)

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
        ${CMAKE_SOURCE_DIR}/frontend/styles.css
        $<TARGET_FILE_DIR:Biomes>
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/frontend/assets $<TARGET_FILE_DIR:Biomes>/assets
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/frontend/logo $<TARGET_FILE_DIR:Biomes>/logo
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/frontend/images $<TARGET_FILE_DIR:Biomes>/images
    DEPENDS Biomes
)
