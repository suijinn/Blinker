#pragma once

#include <windows.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "core/app.h"
#include "win/renderer_d2d.h"

namespace blinker {

// メインウィンドウ。Win32 メッセージを App のコマンド/イベントに変換し、IAppHost を実装する。
class MainWindow final : public IAppHost {
public:
    // ImageCache のワーカースレッドからのデコード完了通知に使う
    static constexpr UINT kMsgImageDecoded = WM_APP + 1;

    bool create(HINSTANCE hinstance, int showCommand, bool darkTitleBar);
    void attachApp(App* app);
    HWND hwnd() const { return hwnd_; }

    // IAppHost
    void requestRedraw() override;
    void setTitle(const std::string& title) override;
    void setFullscreen(bool enabled) override;
    bool isFullscreen() const override { return fullscreen_; }
    std::optional<std::filesystem::path> showOpenDialog() override;
    std::optional<std::filesystem::path> showSaveDialog(
        const std::string& defaultFileName) override;
    std::optional<size_t> showContextMenu(const std::vector<MenuItem>& items,
                                          Point screenPos) override;
    std::optional<std::string> showTextInput(const std::string& initial) override;
    std::optional<uint32_t> showColorPicker(uint32_t initialRGB) override;
    void startTimer(unsigned milliseconds) override;
    void quit() override;

private:
    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handleMessage(UINT msg, WPARAM wp, LPARAM lp);
    void handleLeftDown(POINT pt);  // クリック位置の消費判定とパン/注釈ドラッグの開始
    void onPaint();
    void onSize(uint32_t width, uint32_t height);
    bool handleKey(WPARAM vk);
    void onDropFiles(WPARAM wp);
    static KeyCode keyCodeFromVirtualKey(WPARAM vk);

    HWND hwnd_ = nullptr;
    App* app_ = nullptr;
    std::unique_ptr<RendererD2D> renderer_;
    bool fullscreen_ = false;
    WINDOWPLACEMENT savedPlacement_{sizeof(WINDOWPLACEMENT)};
    LONG savedStyle_ = 0;
    bool dragging_ = false;
    bool rightDragging_ = false;  // 編集領域の選択中(右ボタン)
    POINT lastDragPos_{};
    bool trackingMouseLeave_ = false;
};

} // namespace blinker
