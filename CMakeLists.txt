cmake_minimum_required(VERSION 3.21)
project(Blinker VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# バージョン表示用の git SHA-1(短縮形)。取得できない場合は "unknown"
set(BLINKER_GIT_SHA "unknown")
find_package(Git QUIET)
if(Git_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short=8 HEAD
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE BLINKER_GIT_SHA_RESULT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE BLINKER_GIT_RESULT)
    if(BLINKER_GIT_RESULT EQUAL 0 AND NOT BLINKER_GIT_SHA_RESULT STREQUAL "")
        set(BLINKER_GIT_SHA "${BLINKER_GIT_SHA_RESULT}")
    endif()
endif()
configure_file(src/core/version.h.in
    ${CMAKE_BINARY_DIR}/generated/core/version.h @ONLY)

# core: プラットフォーム非依存層。単体テストからもリンクする
add_library(blinker_core STATIC
    src/core/annotation_edit.cpp
    src/core/app.cpp
    src/core/config.cpp
    src/core/dib.cpp
    src/core/edit.cpp
    src/core/image_cache.cpp
    src/core/image_list.cpp
    src/core/keymap.cpp
    src/core/pixel_convert.cpp
    src/core/unicode.cpp
    src/core/viewport.cpp
)
target_include_directories(blinker_core PUBLIC src ${CMAKE_BINARY_DIR}/generated)
if(MSVC)
    target_compile_options(blinker_core PUBLIC /W4 /utf-8 /permissive-)
else()
    target_compile_options(blinker_core PUBLIC -Wall -Wextra)
endif()
if(NOT WIN32)
    # ImageCache のワーカースレッド (std::thread) 用
    find_package(Threads REQUIRED)
    target_link_libraries(blinker_core PUBLIC Threads::Threads)
endif()

if(WIN32)
    # FileVersion/ProductVersion に PROJECT_VERSION と git SHA-1 を埋め込む
    set(BLINKER_ICON_PATH "${CMAKE_SOURCE_DIR}/assets/blinker.ico")
    configure_file(src/win/blinker.rc.in
        ${CMAKE_BINARY_DIR}/generated/win/blinker.rc @ONLY)

    add_executable(blinker WIN32
        src/win/main_win.cpp
        src/win/window_win.cpp
        src/win/annotation_d2d.cpp
        src/win/annotation_draw.cpp
        src/win/renderer_d2d.cpp
        src/win/decoder_wic.cpp
        src/win/encoder_wic.cpp
        src/win/wic_factory.cpp
        src/win/file_system_win.cpp
        src/win/clipboard_win.cpp
        ${CMAKE_BINARY_DIR}/generated/win/blinker.rc
        src/win/blinker.manifest
    )
    target_compile_definitions(blinker PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(blinker PRIVATE blinker_core
        d2d1 dwrite windowscodecs shlwapi ole32 comdlg32 shell32 user32 gdi32 dwmapi advapi32)
endif()

# SDL3 バックエンド (Linux / macOS)。既定で非 Windows のとき有効。
# システムに SDL3 があればそれを使い、なければ FetchContent でソース取得してビルドする
if(WIN32)
    option(BLINKER_SDL "Build the SDL3 backend" OFF)
else()
    option(BLINKER_SDL "Build the SDL3 backend" ON)
endif()

if(BLINKER_SDL)
    find_package(SDL3 CONFIG QUIET)
    if(NOT TARGET SDL3::SDL3)
        include(FetchContent)
        set(SDL_SHARED OFF CACHE BOOL "" FORCE)
        set(SDL_STATIC ON CACHE BOOL "" FORCE)
        set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(SDL3
            GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
            GIT_TAG release-3.2.16
            GIT_SHALLOW TRUE)
        FetchContent_MakeAvailable(SDL3)
    endif()

    add_executable(blinker_sdl
        src/sdl/main_sdl.cpp
        src/sdl/window_sdl.cpp
        src/sdl/renderer_sdl.cpp
        src/sdl/font_stb.cpp
        src/sdl/decoder_stb.cpp
        src/sdl/encoder_stb.cpp
        src/sdl/file_system_posix.cpp
        src/sdl/clipboard_sdl.cpp
        src/sdl/stb_impl.cpp
    )
    # stb ヘッダはライブラリ由来の警告が出るため、実装 TU のみ警告を抑止する
    set_source_files_properties(src/sdl/stb_impl.cpp PROPERTIES
        COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/w,-w>")
    target_include_directories(blinker_sdl PRIVATE third_party)
    target_link_libraries(blinker_sdl PRIVATE blinker_core SDL3::SDL3)
    if(NOT WIN32)
        # 非 Windows では実行ファイル名は blinker(win 版と同じ使い勝手)
        set_target_properties(blinker_sdl PROPERTIES OUTPUT_NAME blinker)
    endif()
endif()

enable_testing()
add_subdirectory(tests)
