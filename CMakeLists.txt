cmake_minimum_required(VERSION 3.21)
project(Blinker VERSION 0.1.8 LANGUAGES CXX)

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
    src/core/animation.cpp
    src/core/annotation_edit.cpp
    src/core/app.cpp
    src/core/config.cpp
    src/core/dib.cpp
    src/core/edit.cpp
    src/core/edit_drag_state.cpp
    src/core/edit_history.cpp
    src/core/edit_style.cpp
    src/core/exif.cpp
    src/core/help.cpp
    src/core/image_cache.cpp
    src/core/image_list.cpp
    src/core/image_origin.cpp
    src/core/image_scale.cpp
    src/core/keymap.cpp
    src/core/menu.cpp
    src/core/mousemap.cpp
    src/core/nav_arrows.cpp
    src/core/object_drag_state.cpp
    src/core/ocr_service.cpp
    src/core/ocr_text.cpp
    src/core/pixel_convert.cpp
    src/core/pointer_state.cpp
    src/core/print_layout.cpp
    src/core/scan_service.cpp
    src/core/sidebar_state.cpp
    src/core/sort_order.cpp
    src/core/text_edit.cpp
    src/core/text_edit_state.cpp
    src/core/text_style.cpp
    src/core/unicode.cpp
    src/core/viewport.cpp
)
target_include_directories(blinker_core PUBLIC src ${CMAKE_BINARY_DIR}/generated)

if(MSVC)
    target_compile_options(blinker_core PUBLIC /W4 /utf-8 /permissive-)
else()
    # -Wmissing-field-initializers は切る。C++ では既定メンバ初期化子 (NSDMI) を持つ
    # 末尾のメンバを集約初期化で省くのが正しい書き方で (EditMenuEntry の
    # {Action::PickBorderColor} など)、gcc/clang はそれにも鳴る。MSVC /W4 は鳴らない。
    # 埋めて回ると NSDMI を置いた意味が無くなるので、警告の側を黙らせる
    target_compile_options(blinker_core PUBLIC -Wall -Wextra -Wno-missing-field-initializers)
endif()

# 「警告ゼロを維持する」を CI で強制するためのスイッチ。既定は OFF ―― 手元のビルドが
# 警告ひとつで止まると作業しづらいため、落とすのは CI (ci.yml) だけにする。
option(BLINKER_WERROR "Treat compiler warnings as errors (CI only)" OFF)

# 付ける先を CMAKE_CXX_FLAGS ではなくターゲット個別にしているのは、前者だと
# FetchContent で一緒にビルドする SDL3 まで巻き込むから。
#
# 対象は製品コード (blinker_core / blinker / blinker_sdl) だけで、core_tests には
# 付けない。テストには gcc 13 が -O3 で誤検知する警告が出るため:
#   - -Wdangling-reference … const AnnotationSpec& s = app.annotations().specs->back();
#     AnnotationsView::specs は App が持つ vector への生ポインタなので、view の一時
#     オブジェクトが消えても参照先は生きている(GCC 14 で誤検知は緩和された)
#   - -Wstringop-overflow … std::vector::insert の内部を -O3 でインライン展開した先
# どちらもコンパイラの版で増減する類のもので、これで CI を落としても得るものがない。
# 製品コードのほうは MSVC・gcc とも警告ゼロなので、そちらは厳密に守る
function(blinker_werror target)
    if(NOT BLINKER_WERROR)
        return()
    endif()
    if(MSVC)
        target_compile_options(${target} PRIVATE /WX)
    else()
        target_compile_options(${target} PRIVATE -Werror)
    endif()
endfunction()

blinker_werror(blinker_core)
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
    # .rc は .ico を参照するだけなので、明示しないと差し替えても再ビルドされない
    set_source_files_properties(${CMAKE_BINARY_DIR}/generated/win/blinker.rc
        PROPERTIES OBJECT_DEPENDS "${BLINKER_ICON_PATH}")

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
        src/win/ocr_winrt.cpp
        src/win/printer_win.cpp
        ${CMAKE_BINARY_DIR}/generated/win/blinker.rc
        src/win/blinker.manifest
    )
    target_compile_definitions(blinker PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(blinker PRIVATE blinker_core
        d2d1 dwrite windowscodecs shlwapi ole32 comdlg32 shell32 user32 gdi32 dwmapi advapi32
        imm32)
    blinker_werror(blinker)
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
    # stb_impl.cpp は上で /w・-w を付けてあり、それが -Werror より後に来るので影響しない
    blinker_werror(blinker_sdl)
    if(NOT WIN32)
        # 非 Windows では実行ファイル名は blinker(win 版と同じ使い勝手)
        set_target_properties(blinker_sdl PROPERTIES OUTPUT_NAME blinker)
    endif()
endif()

enable_testing()
add_subdirectory(tests)
