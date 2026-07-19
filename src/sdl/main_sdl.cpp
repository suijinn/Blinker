// SDL3 バックエンドのエントリポイント(Linux / macOS)。
// 構成は win 版 (main_win.cpp) と同型: 依存を組み立てて App に注入し、
// イベントループを回す。

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <filesystem>

#include "core/app.h"
#include "core/config.h"
#include "core/image_cache.h"
#include "core/str_util.h"
#include "core/unicode.h"
#include "sdl/annotation_stub.h"
#include "sdl/clipboard_sdl.h"
#include "sdl/decoder_stb.h"
#include "sdl/encoder_stb.h"
#include "sdl/file_system_posix.h"
#include "sdl/font_stb.h"
#include "sdl/window_sdl.h"

namespace {

// [view] theme = auto | dark | light。auto は OS のテーマ設定に追従する
bool resolveDarkTheme(const blinker::Config& config) {
    const std::string theme = blinker::toLower(blinker::trim(config.get("view", "theme", "auto")));
    if (theme == "dark") return true;
    if (theme == "light") return false;
    return SDL_GetSystemTheme() != SDL_SYSTEM_THEME_LIGHT;  // 不明時はダーク
}

std::filesystem::path exeDirectory() {
    const char* base = SDL_GetBasePath();  // SDL が所有(解放不要)
    return base ? blinker::pathFromUtf8(base) : std::filesystem::path{};
}

} // namespace

int main(int argc, char** argv) {
    using namespace blinker;

    SDL_SetAppMetadata("Blinker", "0.1.0", "dev.blinker.viewer");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    int exitCode = 0;
    {
        const Config config = Config::loadFile(exeDirectory() / "blinker.ini");

        DecoderStb decoder;
        ImageCache cache(decoder);
        FileSystemPosix fileSystem;

        FontStb font;
        if (!font.load(config.get("view", "font_path"))) {
            // フォントがなくても画像表示は動く(サイドバー・ステータスバーの文字が出ない)
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "UI フォントが見つかりません。blinker.ini の [view] font_path で"
                        "フォントファイル (.ttf/.ttc/.otf) を指定してください");
        }

        WindowSdl window;
        if (!window.create(font)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ウィンドウ生成に失敗: %s",
                         SDL_GetError());
            SDL_Quit();
            return 1;
        }

        ClipboardSdl clipboard;
        EncoderStb encoder;
        AnnotationStub annotationRasterizer;

        App app(window, fileSystem, cache, clipboard, encoder, annotationRasterizer);
        app.setDarkTheme(resolveDarkTheme(config));
        app.applyConfig(config);
        window.attachApp(&app);

        cache.setOnDecoded(
            [&window](const std::filesystem::path&) { window.postDecodedEvent(); });

        // POSIX の argv はバイト列。ロケールは UTF-8 前提(現代の Linux/macOS 標準)
        if (argc >= 2 && argv[1] && argv[1][0] != '\0') {
            app.openPath(pathFromUtf8(argv[1]));
        }

        window.run();
    }  // cache のデストラクタがワーカースレッドを join してから SDL を終了する

    SDL_Quit();
    return exitCode;
}
