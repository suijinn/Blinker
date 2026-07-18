#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "core/command.h"
#include "core/config.h"
#include "core/geometry.h"
#include "core/image_cache.h"
#include "core/image_list.h"
#include "core/keymap.h"
#include "core/viewport.h"
#include "platform/clipboard.h"
#include "platform/encoder.h"
#include "platform/file_system.h"
#include "platform/renderer.h"

namespace blinker {

// App がウィンドウ層に要求するサービス。win 層 (MainWindow) が実装する。
class IAppHost {
public:
    virtual ~IAppHost() = default;
    virtual void requestRedraw() = 0;
    virtual void setTitle(const std::wstring& title) = 0;
    virtual void setFullscreen(bool enabled) = 0;
    virtual bool isFullscreen() const = 0;
    virtual std::optional<std::filesystem::path> showOpenDialog() = 0;
    // 保存ダイアログ。キャンセル時 nullopt。返るパスには拡張子が付いていること
    virtual std::optional<std::filesystem::path> showSaveDialog(
        const std::wstring& defaultFileName) = 0;
    virtual void startTimer(unsigned milliseconds) = 0;  // 単発。満了で App::onTimer が呼ばれる
    virtual void quit() = 0;
};

// アプリ本体の状態機械。入力は Command に正規化されて execute() に集まり、
// 状態を更新して host にタイトル変更・再描画を依頼する(一方向フロー)。
class App {
public:
    App(IAppHost& host, IFileSystem& fileSystem, ImageCache& cache, IClipboard& clipboard,
        IImageEncoder& encoder);

    void applyConfig(const Config& config);
    void setDarkTheme(bool dark) { darkTheme_ = dark; }  // ステータスバー配色に反映

    void openPath(const std::filesystem::path& path);  // 画像ファイル or フォルダ
    void execute(Command command);
    bool onKey(const KeyChord& chord);  // バインドがあれば実行して true
    void onResize(float width, float height);
    void onWheel(float wheelNotches, Point screenPos);  // 正で拡大。サイドバー上ではスクロール
    bool onMouseDown(Point screenPos);  // サイドバーのクリックを消費したら true(パンを開始しない)
    void onDragPan(float dx, float dy);
    void onMouseMove(Point screenPos);  // ステータスバーの座標・色表示を更新
    void onMouseLeave();
    void onTimer();                // host のタイマー満了(通知メッセージを消す)
    void onDecodeCompleted();  // UI スレッドで呼ぶこと

    // 描画用スナップショット
    const std::shared_ptr<DecodedImage>& currentImage() const { return current_; }
    Matrix3x2 imageToScreen() const;  // サイドバー分のオフセットを含む
    float zoom() const { return viewport_.zoom(); }
    uint32_t backgroundRGB() const { return backgroundRGB_; }
    StatusBarView statusBar() const;
    SidebarView sidebar() const;

private:
    static constexpr float kPanStepPx = 64.0f;
    static constexpr float kStatusBarHeight = 26.0f;
    static constexpr float kSidebarItemHeight = 24.0f;

    void refreshCurrent();
    void onViewChanged();
    void updatePrefetch();
    void updateTitle();
    bool statusBarVisible() const;
    bool sidebarVisible() const;
    float sidebarOffset() const;      // サイドバー幅。非表示なら 0
    float sidebarViewHeight() const;  // サイドバー領域の高さ(ステータスバーを除く)
    void clampSidebarScroll();
    void scrollSidebarToCurrent();    // 現在項目が見える位置までスクロール
    void applyLayout();  // サイドバー・ステータスバーの分だけビューポートを狭める
    std::wstring hoverInfoText(Point screenPos) const;
    void showMessage(std::wstring text);

    IAppHost& host_;
    IFileSystem& fileSystem_;
    ImageCache& cache_;
    IClipboard& clipboard_;
    IImageEncoder& encoder_;
    Keymap keymap_ = Keymap::defaults();
    ImageList list_;
    Viewport viewport_;
    std::shared_ptr<DecodedImage> current_;
    std::filesystem::path displayedPath_;  // current_ がどのパスの画像か
    bool clipboardImage_ = false;  // current_ が貼り付け画像(フォルダ一覧とは独立)
    bool loadFailed_ = false;
    uint32_t backgroundRGB_ = 0x202020;
    int prefetchRadius_ = 2;
    SizeF clientSize_{};        // クライアント領域全体(サイドバー + ビューポート + ステータスバー)
    bool statusBarEnabled_ = true;
    bool sidebarEnabled_ = false;
    float sidebarWidth_ = 220.0f;
    float sidebarScroll_ = 0.0f;  // 一覧のスクロール量 (px)
    bool darkTheme_ = true;
    std::wstring message_;      // ステータスバー左側の通知(タイマーで消える)
    std::wstring hoverText_;    // ステータスバー右側(カーソル位置の座標・色)
};

} // namespace blinker
