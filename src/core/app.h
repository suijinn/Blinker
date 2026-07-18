#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/command.h"
#include "core/config.h"
#include "core/geometry.h"
#include "core/image_cache.h"
#include "core/image_list.h"
#include "core/keymap.h"
#include "core/viewport.h"
#include "platform/annotation.h"
#include "platform/clipboard.h"
#include "platform/encoder.h"
#include "platform/file_system.h"
#include "platform/renderer.h"

namespace blinker {

// ポップアップメニューの1項目。children が空でなければサブメニューになる。
// separator = true の項目は区切り線(text・children は無視)。
struct MenuItem {
    std::wstring text;
    bool checked = false;
    bool separator = false;
    std::vector<MenuItem> children;
};

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
    // ポップアップメニューをクライアント座標 screenPos に表示し、選択された項目の
    // index を返す(キャンセル時 nullopt)。index は選択可能な末端項目(separator と
    // サブメニュー親を除く)を深さ優先で数えた通し番号。モーダル(選択されるまで返らない)
    virtual std::optional<size_t> showContextMenu(const std::vector<MenuItem>& items,
                                                  Point screenPos) = 0;
    // テキスト入力ダイアログ(複数行)。キャンセル時 nullopt。改行は L'\n'
    virtual std::optional<std::wstring> showTextInput() = 0;
    // 色選択ダイアログ。キャンセル時 nullopt
    virtual std::optional<uint32_t> showColorPicker(uint32_t initialRGB) = 0;
    virtual void startTimer(unsigned milliseconds) = 0;  // 単発。満了で App::onTimer が呼ばれる
    virtual void quit() = 0;
};

// アプリ本体の状態機械。入力は Command に正規化されて execute() に集まり、
// 状態を更新して host にタイトル変更・再描画を依頼する(一方向フロー)。
class App {
public:
    App(IAppHost& host, IFileSystem& fileSystem, ImageCache& cache, IClipboard& clipboard,
        IImageEncoder& encoder, IAnnotationRasterizer& rasterizer);

    void applyConfig(const Config& config);
    void setDarkTheme(bool dark) { darkTheme_ = dark; }  // ステータスバー配色に反映

    void openPath(const std::filesystem::path& path);  // 画像ファイル or フォルダ
    void execute(Command command);
    bool onKey(const KeyChord& chord);  // バインドがあれば実行して true
    void onResize(float width, float height);
    void onWheel(float wheelNotches, Point screenPos);  // 正で拡大。サイドバー上ではスクロール
    bool onMouseDown(Point screenPos);  // サイドバーのクリックを消費したら true(パンを開始しない)
    void onRightDragStart(Point screenPos);  // 編集領域の選択開始(画像外・サイドバー上は無視)
    void onRightDragMove(Point screenPos);
    void onRightDragEnd(Point screenPos);  // メニューを表示し、選ばれた編集を適用する
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
    SelectionView selection() const;  // 選択中のラバーバンド(スクリーン座標)

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
    Point clampToImage(Point imagePos) const;

    // 編集メニューの末端項目が表す操作。設定系はメニューを再表示して続けて選択できる
    struct EditMenuEntry {
        enum class Action { Crop, Annotate, StrokeWidth, FontSize, Angle, PickColor };
        Action action;
        AnnotationSpec::Kind kind = AnnotationSpec::Kind::Rect;  // Annotate 用
        float value = 0;                                         // 設定系の値
    };
    // メニュー構造を組み立てる。entries[i] が末端項目 i(showContextMenu の返す index)に対応
    std::vector<MenuItem> buildEditMenu(std::vector<EditMenuEntry>& entries) const;
    bool applyEditChoice(const EditMenuEntry& entry);  // true ならメニューを閉じる
    void applyCrop();
    void applyAnnotation(AnnotationSpec::Kind kind);
    void pushUndo();       // 現在の画像を undo 履歴へ積む(上限あり)
    void executeUndo();
    void discardEdits();   // 画像切替時に編集を破棄する(あれば通知)

    IAppHost& host_;
    IFileSystem& fileSystem_;
    ImageCache& cache_;
    IClipboard& clipboard_;
    IImageEncoder& encoder_;
    IAnnotationRasterizer& rasterizer_;
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

    // 編集(トリミング・図形・テキスト)の状態
    static constexpr float kDragThresholdPx = 4.0f;  // これ未満の右ドラッグは無視(画面px)
    static constexpr size_t kUndoLimit = 10;
    bool selecting_ = false;    // 右ドラッグで領域選択中
    Point selStartImage_{};     // 選択の始点・現在点(画像座標。ズーム中も不変)
    Point selEndImage_{};
    Point selStartScreen_{};    // ドラッグ量の閾値判定用
    bool edited_ = false;       // current_ に未保存の編集がある
    std::vector<std::shared_ptr<DecodedImage>> undoStack_;
    uint32_t editColorRGB_ = 0xFF3B30;
    float editStrokeWidth_ = 3.0f;  // 画面px基準(適用時に 1/zoom で画像座標へ換算)
    float editFontSize_ = 18.0f;    // 同上
    float editAngleDeg_ = 0.0f;     // 図形・テキストの回転角(時計回り、度)
};

} // namespace blinker
