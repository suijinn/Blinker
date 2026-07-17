cmake_minimum_required(VERSION 3.21)
project(Blinker VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# core: プラットフォーム非依存層。単体テストからもリンクする
add_library(blinker_core STATIC
    src/core/app.cpp
    src/core/config.cpp
    src/core/image_cache.cpp
    src/core/image_list.cpp
    src/core/keymap.cpp
    src/core/viewport.cpp
)
target_include_directories(blinker_core PUBLIC src)
if(MSVC)
    target_compile_options(blinker_core PUBLIC /W4 /utf-8 /permissive-)
endif()

if(WIN32)
    add_executable(blinker WIN32
        src/win/main_win.cpp
        src/win/window_win.cpp
        src/win/renderer_d2d.cpp
        src/win/decoder_wic.cpp
        src/win/file_system_win.cpp
        src/win/clipboard_win.cpp
        src/win/blinker.rc
        src/win/blinker.manifest
    )
    target_compile_definitions(blinker PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(blinker PRIVATE blinker_core
        d2d1 dwrite windowscodecs shlwapi ole32 comdlg32 shell32 user32 gdi32 dwmapi advapi32)
endif()

enable_testing()
add_subdirectory(tests)
