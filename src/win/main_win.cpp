#include <windows.h>

#include <objbase.h>
#include <shellapi.h>

#include <filesystem>

#include "core/app.h"
#include "core/config.h"
#include "core/image_cache.h"
#include "core/str_util.h"
#include "win/clipboard_win.h"
#include "win/decoder_wic.h"
#include "win/encoder_wic.h"
#include "win/file_system_win.h"
#include "win/window_win.h"

namespace {

// [view] theme = auto | dark | light。auto は OS のアプリテーマ設定に追従する
bool resolveDarkTheme(const blinker::Config& config) {
    const std::string theme = blinker::toLower(blinker::trim(config.get("view", "theme", "auto")));
    if (theme == "dark") return true;
    if (theme == "light") return false;
    DWORD lightTheme = 1;
    DWORD size = sizeof(lightTheme);
    if (RegGetValueW(HKEY_CURRENT_USER,
                     L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &lightTheme,
                     &size) != ERROR_SUCCESS) {
        return false;  // 読めない環境ではライト(OS既定)のまま
    }
    return lightTheme == 0;
}

std::filesystem::path exeDirectory() {
    wchar_t buffer[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    return std::filesystem::path(buffer).parent_path();
}

std::filesystem::path pathFromCommandLine() {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::filesystem::path result;
    if (argv && argc >= 2) result = argv[1];
    if (argv) LocalFree(argv);
    return result;
}

} // namespace

int WINAPI wWinMain(HINSTANCE hinstance, HINSTANCE, PWSTR, int showCommand) {
    using namespace blinker;

    // メインスレッドは STA(ファイルダイアログ等のため)。WIC デコードはワーカースレッド側
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) {
        return 1;
    }

    int exitCode = 0;
    {
        const Config config = Config::loadFile(exeDirectory() / L"blinker.ini");

        DecoderWic decoder;
        ImageCache cache(decoder);
        FileSystemWin fileSystem;

        const bool darkTheme = resolveDarkTheme(config);
        MainWindow window;
        if (!window.create(hinstance, showCommand, darkTheme)) {
            CoUninitialize();
            return 1;
        }

        ClipboardWin clipboard;
        clipboard.setOwner(window.hwnd());
        EncoderWic encoder;

        App app(window, fileSystem, cache, clipboard, encoder);
        app.setDarkTheme(darkTheme);
        app.applyConfig(config);
        window.attachApp(&app);

        cache.setOnDecoded([hwnd = window.hwnd()](const std::filesystem::path&) {
            PostMessageW(hwnd, MainWindow::kMsgImageDecoded, 0, 0);
        });

        if (const auto initialPath = pathFromCommandLine(); !initialPath.empty()) {
            app.openPath(initialPath);
        }

        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        exitCode = static_cast<int>(msg.wParam);
    }  // cache のデストラクタがワーカースレッドを join してから COM を解放する

    CoUninitialize();
    return exitCode;
}
