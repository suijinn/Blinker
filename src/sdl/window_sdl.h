#pragma once

#include <SDL3/SDL.h>

#include <memory>
#include <optional>
#include <string>

#include "core/app.h"
#include "sdl/font_stb.h"
#include "sdl/renderer_sdl.h"

namespace blinker {

// メインウィンドウ(SDL バックエンド)。SDL イベントを App のコマンド/イベントに
// 変換し、IAppHost を実装する。
// 未対応の IAppHost API(コンテキストメニュー・テキスト入力・色選択)は nullopt を
// 返し、編集機能は無効のまま閲覧機能をフルに提供する。
class WindowSdl final : public IAppHost {
public:
    bool create(FontStb& font);
    void attachApp(App* app);
    void run();  // イベントループ(quit まで戻らない)

    // ImageCache のワーカースレッドから呼べる(SDL_PushEvent はスレッド安全)
    void postDecodedEvent();

    // IAppHost
    void requestRedraw() override { needsRedraw_ = true; }
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
    // ファイルダイアログ(非同期コールバック)の完了待ちに使う
    struct DialogState;

    void handleEvent(const SDL_Event& event);
    void renderIfNeeded();
    Point toPixels(float x, float y) const;  // ウィンドウ座標 → 物理ピクセル
    static KeyCode keyCodeFromSdl(SDL_Keycode key, SDL_Keymod mod);
    std::optional<std::filesystem::path> waitForDialog(DialogState& state);

    SDL_Window* window_ = nullptr;
    SDL_Renderer* sdlRenderer_ = nullptr;
    std::unique_ptr<RendererSdl> renderer_;
    App* app_ = nullptr;
    Uint32 eventDecoded_ = 0;  // SDL_RegisterEvents で確保したユーザーイベント
    Uint32 eventTimer_ = 0;
    SDL_TimerID timerId_ = 0;
    bool running_ = false;
    bool needsRedraw_ = false;
    bool fullscreen_ = false;
    bool dragging_ = false;        // 左ドラッグ(パン)
    bool rightDragging_ = false;   // 右ドラッグ(領域選択)
    float lastDragX_ = 0, lastDragY_ = 0;  // 物理ピクセル
};

} // namespace blinker
