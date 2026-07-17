#include <windows.h>

#include <objbase.h>
#include <shellapi.h>

#include <filesystem>

#include "core/app.h"
#include "core/config.h"
#include "core/image_cache.h"
#include "win/decoder_wic.h"
#include "win/file_system_win.h"
#include "win/window_win.h"

namespace {

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

        MainWindow window;
        if (!window.create(hinstance, showCommand)) {
            CoUninitialize();
            return 1;
        }

        App app(window, fileSystem, cache);
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
