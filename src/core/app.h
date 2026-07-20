#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/annotation_edit.h"
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

/**
 * @file app.h
 * @brief アプリ本体の状態機械と、ウィンドウ層へのサービス要求インターフェース。
 */

namespace blinker {

/**
 * @brief ポップアップメニューの 1 項目。
 *
 * children が空でなければサブメニューになる。separator = true の項目は区切り線
 * (text・children は無視される)。
 */
struct MenuItem {
    std::string text;                 ///< 表示文字列(UTF-8)
    bool checked = false;             ///< チェックマークを付けるか
    bool separator = false;           ///< 区切り線として扱うか
    std::vector<MenuItem> children;   ///< サブメニューの項目(空なら末端項目)
};

/**
 * @brief App がウィンドウ層に要求するサービス。
 *
 * win 層 (MainWindow) と sdl 層 (WindowSdl) が実装する。
 */
class IAppHost {
public:
    virtual ~IAppHost() = default;

    /// @brief 再描画を要求する(実際の描画は後続の描画イベントで行われる)。
    virtual void requestRedraw() = 0;

    /**
     * @brief ウィンドウタイトルを設定する。
     * @param[in] title 設定するタイトル(UTF-8)。
     */
    virtual void setTitle(const std::string& title) = 0;

    /**
     * @brief フルスクリーン表示を切り替える。
     * @param[in] enabled true でフルスクリーン、false で通常ウィンドウ。
     */
    virtual void setFullscreen(bool enabled) = 0;

    /**
     * @brief 現在フルスクリーンかを返す。
     * @return フルスクリーンなら true。
     */
    virtual bool isFullscreen() const = 0;

    /**
     * @brief ファイルを開くダイアログを表示する。
     * @return 選択されたパス。キャンセル時は std::nullopt。
     */
    virtual std::optional<std::filesystem::path> showOpenDialog() = 0;

    /**
     * @brief 名前を付けて保存ダイアログを表示する。
     * @param[in] defaultFileName 初期表示するファイル名(UTF-8)。
     * @return 選択されたパス(拡張子付き)。キャンセル時は std::nullopt。
     */
    virtual std::optional<std::filesystem::path> showSaveDialog(
        const std::string& defaultFileName) = 0;

    /**
     * @brief ポップアップメニューを表示する(モーダル。選択されるまで返らない)。
     * @param[in] items     メニュー構造。
     * @param[in] screenPos 表示位置(クライアント座標)。
     * @return 選択された末端項目の index。キャンセル時は std::nullopt。
     *         index は選択可能な末端項目(separator とサブメニュー親を除く)を
     *         深さ優先で数えた通し番号。
     */
    virtual std::optional<size_t> showContextMenu(const std::vector<MenuItem>& items,
                                                  Point screenPos) = 0;

    /**
     * @brief テキスト入力ダイアログ(複数行)を表示する。
     * @param[in] initial 初期値として表示する文字列(UTF-8、改行は LF)。再編集用。
     * @return 入力された文字列(UTF-8、改行は LF)。キャンセル時は std::nullopt。
     */
    virtual std::optional<std::string> showTextInput(const std::string& initial) = 0;

    /**
     * @brief 色選択ダイアログを表示する。
     * @param[in] initialRGB 初期選択する色(0xRRGGBB)。
     * @return 選択された色(0xRRGGBB)。キャンセル時は std::nullopt。
     */
    virtual std::optional<uint32_t> showColorPicker(uint32_t initialRGB) = 0;

    /**
     * @brief 単発タイマーを開始する。
     * @param[in] milliseconds 満了までの時間(ミリ秒)。満了で App::onTimer が呼ばれる。
     */
    virtual void startTimer(unsigned milliseconds) = 0;

    /// @brief アプリケーションの終了を要求する。
    virtual void quit() = 0;
};

/**
 * @brief アプリ本体の状態機械。
 *
 * 入力は Command に正規化されて execute() に集まり、状態を更新して host に
 * タイトル変更・再描画を依頼する(一方向フロー)。UI スレッド専用でスレッド安全ではない。
 */
class App {
public:
    /**
     * @brief 依存オブジェクトを受け取って構築する。
     * @param[in] host       ウィンドウ層のサービス。本オブジェクトより長生きすること。
     * @param[in] fileSystem ファイル列挙の実装。
     * @param[in] cache      デコード済み画像のキャッシュ。
     * @param[in] clipboard  クリップボードの実装。
     * @param[in] encoder    画像保存の実装。
     * @param[in] rasterizer 注釈ラスタライズの実装。
     */
    App(IAppHost& host, IFileSystem& fileSystem, ImageCache& cache, IClipboard& clipboard,
        IImageEncoder& encoder, IAnnotationRasterizer& rasterizer);

    /**
     * @brief blinker.ini の設定を適用する。
     * @param[in] config 適用する設定。キーバインド・背景色・先読み数などを反映する。
     */
    void applyConfig(const Config& config);

    /**
     * @brief ダークテーマかどうかを設定する。
     * @param[in] dark true ならダーク配色。ステータスバー・サイドバーの配色に反映される。
     */
    void setDarkTheme(bool dark) { darkTheme_ = dark; }

    /**
     * @brief 画像ファイルまたはフォルダを開く。
     * @param[in] path 画像ファイルのパス、またはフォルダのパス。
     *                 フォルダなら先頭の画像を表示する。
     */
    void openPath(const std::filesystem::path& path);

    /**
     * @brief コマンドを実行して状態を更新する。
     * @param[in] command 実行するコマンド。
     */
    void execute(Command command);

    /**
     * @brief キー入力を処理する。
     * @param[in] chord 入力されたキー。
     * @return バインドがあり実行したら true。未バインドなら false。
     */
    bool onKey(const KeyChord& chord);

    /**
     * @brief クライアント領域のサイズ変更を通知する。
     * @param[in] width  新しい幅(物理ピクセル)。
     * @param[in] height 新しい高さ(物理ピクセル)。
     */
    void onResize(float width, float height);

    /**
     * @brief ホイール操作を処理する。
     * @param[in] wheelNotches 回転量(ノッチ単位)。正で拡大。
     * @param[in] screenPos    ポインタ位置。サイドバー上ならズームではなくスクロールする。
     */
    void onWheel(float wheelNotches, Point screenPos);

    /**
     * @brief 左ボタン押下を処理する。
     * @param[in] screenPos 押下位置(スクリーン座標)。
     * @return サイドバーのクリックや注釈の選択・ドラッグ開始を消費したら true
     *         (呼び出し側はパンを開始しないこと)。
     */
    bool onMouseDown(Point screenPos);

    /// @brief 左ボタン解放を処理し、注釈の移動・回転ドラッグを終了する。
    void onMouseUp();

    /**
     * @brief ダブルクリックを処理する。
     * @param[in] screenPos クリック位置(スクリーン座標)。
     * @return Text 注釈上で再編集を開始したら true。
     */
    bool onDoubleClick(Point screenPos);

    /**
     * @brief 右ドラッグによる編集領域の選択を開始する。
     * @param[in] screenPos 開始位置(スクリーン座標)。画像外・サイドバー上は無視する。
     */
    void onRightDragStart(Point screenPos);

    /**
     * @brief 右ドラッグ中の選択領域を更新する。
     * @param[in] screenPos 現在位置(スクリーン座標)。
     */
    void onRightDragMove(Point screenPos);

    /**
     * @brief 右ドラッグを終了し、編集メニューを表示して選ばれた編集を適用する。
     * @param[in] screenPos 終了位置(スクリーン座標)。メニューの表示位置にもなる。
     */
    void onRightDragEnd(Point screenPos);

    /**
     * @brief 左ドラッグによるパンを処理する。
     * @param[in] dx X 方向の移動量(スクリーン px)。
     * @param[in] dy Y 方向の移動量(スクリーン px)。
     */
    void onDragPan(float dx, float dy);

    /**
     * @brief ポインタ移動を処理する。
     * @param[in] screenPos 現在位置(スクリーン座標)。
     * @param[in] shift     Shift が押されているか。回転・リサイズのスナップに使う。
     * @note 注釈ドラッグ中は移動・回転を適用する。それ以外はステータスバーの
     *       座標・色表示を更新する。
     */
    void onMouseMove(Point screenPos, bool shift = false);

    /// @brief ポインタがウィンドウから出たことを通知し、座標・色表示を消す。
    void onMouseLeave();

    /// @brief host のタイマー満了を通知する(ステータスバーの通知メッセージを消す)。
    void onTimer();

    /**
     * @brief デコード完了を通知する。
     * @note UI スレッドで呼ぶこと(ワーカースレッドから直接呼んではならない)。
     */
    void onDecodeCompleted();

    /**
     * @brief 表示中の画像を返す(描画用スナップショット)。
     * @return 表示中の画像。未読み込み・読み込み失敗なら nullptr。
     */
    const std::shared_ptr<DecodedImage>& currentImage() const { return current_; }

    /**
     * @brief 画像座標 → スクリーン座標の変換行列を返す。
     * @return サイドバー分のオフセットを含んだ変換行列。
     */
    Matrix3x2 imageToScreen() const;

    /**
     * @brief 現在のズーム倍率を返す。
     * @return ズーム倍率(1.0 が等倍)。
     */
    float zoom() const { return viewport_.zoom(); }

    /**
     * @brief 背景色を返す。
     * @return 背景色(0xRRGGBB)。
     */
    uint32_t backgroundRGB() const { return backgroundRGB_; }

    /**
     * @brief ステータスバーの描画内容を組み立てる。
     * @return 描画用のスナップショット。
     */
    StatusBarView statusBar() const;

    /**
     * @brief サイドバーの描画内容を組み立てる。
     * @return 可視範囲の項目だけを含む描画用スナップショット。
     */
    SidebarView sidebar() const;

    /**
     * @brief 選択中のラバーバンドの描画内容を組み立てる。
     * @return 選択領域(スクリーン座標)。選択中でなければ visible = false。
     */
    SelectionView selection() const;

    /**
     * @brief 注釈オブジェクトの描画内容を組み立てる。
     * @return 注釈一覧と選択状態(画像座標)。
     */
    AnnotationsView annotations() const;

private:
    static constexpr float kPanStepPx = 64.0f;         ///< パンコマンド 1 回の移動量
    static constexpr float kStatusBarHeight = 26.0f;   ///< ステータスバーの高さ
    static constexpr float kSidebarItemHeight = 24.0f; ///< サイドバー 1 項目の高さ

    /// @brief 現在位置の画像をキャッシュから取り直し、表示状態を更新する。
    void refreshCurrent();

    /// @brief 表示変換の変更をタイトル・再描画へ反映する。
    void onViewChanged();

    /// @brief 現在位置の近傍を先読み候補としてキャッシュへ渡す。
    void updatePrefetch();

    /// @brief ファイル名・位置・ズームからタイトルを組み立てて host に設定する。
    void updateTitle();

    /**
     * @brief ステータスバーを表示するかを返す。
     * @return 表示するなら true(フルスクリーン時は非表示)。
     */
    bool statusBarVisible() const;

    /**
     * @brief サイドバーを表示するかを返す。
     * @return 表示するなら true。
     */
    bool sidebarVisible() const;

    /**
     * @brief サイドバーの占める幅を返す。
     * @return サイドバー幅(px)。非表示なら 0。
     */
    float sidebarOffset() const;

    /**
     * @brief サイドバー領域の高さを返す。
     * @return ステータスバーを除いた高さ(px)。
     */
    float sidebarViewHeight() const;

    /// @brief サイドバーのスクロール量を有効範囲へ収める。
    void clampSidebarScroll();

    /// @brief 現在項目が見える位置までサイドバーをスクロールする。
    void scrollSidebarToCurrent();

    /// @brief サイドバー・ステータスバーの分だけビューポートを狭める。
    void applyLayout();

    /**
     * @brief カーソル位置の情報文字列を組み立てる。
     * @param[in] screenPos カーソル位置(スクリーン座標)。
     * @return 画像座標と色を表す文字列(UTF-8)。画像外なら空文字列。
     */
    std::string hoverInfoText(Point screenPos) const;

    /**
     * @brief ステータスバーに通知メッセージを表示する(一定時間で消える)。
     * @param[in] text 表示する文字列(UTF-8)。所有権を受け取る。
     */
    void showMessage(std::string text);

    /**
     * @brief 画像座標を画像の範囲内へ丸める。
     * @param[in] imagePos 丸める前の座標(画像座標)。
     * @return 画像内へクランプされた座標。
     */
    Point clampToImage(Point imagePos) const;

    /**
     * @brief 編集メニューの末端項目が表す操作。
     * @note 設定系(StrokeWidth/FontSize/PickColor)はメニューを再表示して続けて選択できる。
     */
    struct EditMenuEntry {
        /// @brief 末端項目が表す操作の種類。
        enum class Action { Crop, Annotate, StrokeWidth, FontSize, PickColor };
        Action action;                                           ///< 操作の種類
        AnnotationSpec::Kind kind = AnnotationSpec::Kind::Rect;  ///< Annotate 用の図形種別
        float value = 0;                                         ///< 設定系の値
    };

    /// @brief 注釈オブジェクトを右クリックしたときのメニューの末端項目。
    struct ObjectMenuEntry {
        /// @brief 末端項目が表す操作の種類。
        enum class Action { EditText, Delete, Angle, StrokeWidth, FontSize, PickColor };
        Action action;    ///< 操作の種類
        float value = 0;  ///< Angle/StrokeWidth/FontSize の値
    };

    /**
     * @brief 編集メニューの構造を組み立てる。
     * @param[out] entries 末端項目の一覧。entries[i] が showContextMenu の返す index i に対応する。
     * @return メニュー構造。
     */
    std::vector<MenuItem> buildEditMenu(std::vector<EditMenuEntry>& entries) const;

    /**
     * @brief 編集メニューで選ばれた項目を適用する。
     * @param[in] entry 選択された末端項目。
     * @return メニューを閉じるなら true。設定系で再表示するなら false。
     */
    bool applyEditChoice(const EditMenuEntry& entry);

    /**
     * @brief 注釈オブジェクトのメニュー構造を組み立てる。
     * @param[in]  spec    対象の注釈。表示する項目の内容に反映する。
     * @param[out] entries 末端項目の一覧。entries[i] が showContextMenu の返す index i に対応する。
     * @return メニュー構造。
     */
    std::vector<MenuItem> buildObjectMenu(const AnnotationSpec& spec,
                                          std::vector<ObjectMenuEntry>& entries) const;

    /**
     * @brief 選択中の注釈のメニューを表示し、選ばれた操作を適用する。
     * @param[in] screenPos メニューの表示位置(スクリーン座標)。
     */
    void showObjectMenu(Point screenPos);

    /// @brief 選択領域で現在の画像をトリミングする。
    void applyCrop();

    /**
     * @brief 選択領域に注釈オブジェクトを追加する。
     * @param[in] kind 追加する注釈の種別。
     */
    void applyAnnotation(AnnotationSpec::Kind kind);

    /**
     * @brief Text 注釈の実測サイズを求めて p2 へ反映する(ヒットテスト・選択枠用)。
     * @param[in,out] spec 対象の注釈。成功時に p2 が更新される。
     * @return 実測できたら true。ラスタライズに失敗したら false。
     */
    bool measureTextExtent(AnnotationSpec& spec);

    /**
     * @brief テキスト入力ダイアログで Text 注釈を再編集する。
     * @param[in] index 編集する注釈の index。
     */
    void editAnnotationText(size_t index);

    /// @brief 選択中の注釈オブジェクトを削除する。
    void deleteSelectedAnnotation();

    /**
     * @brief 注釈を合成した保存・コピー用の画像を作る。
     * @return 注釈を焼き込んだ画像。注釈がなければ current_ をそのまま返す。
     */
    std::shared_ptr<DecodedImage> compositeImage() const;

    /// @brief 未保存の編集ありとして記録し、タイトルを更新する。
    void markEdited();

    /// @brief 現在の画像と注釈一覧を undo 履歴へ積む(kUndoLimit を超えた分は捨てる)。
    void pushUndo();

    /// @brief ドラッグ(移動・回転)の最初の変更時に 1 回だけ undo 履歴へ積む。
    void pushDragUndoOnce();

    /// @brief undo 履歴から 1 つ戻す。
    void executeUndo();

    /// @brief 画像切替時に編集を破棄する(編集があれば通知を出す)。
    void discardEdits();

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
    std::filesystem::path displayedPath_;  ///< current_ がどのパスの画像か
    bool clipboardImage_ = false;  ///< current_ が貼り付け画像(フォルダ一覧とは独立)
    bool loadFailed_ = false;
    uint32_t backgroundRGB_ = 0x202020;
    int prefetchRadius_ = 2;
    SizeF clientSize_{};  ///< クライアント領域全体(サイドバー + ビューポート + ステータスバー)
    bool statusBarEnabled_ = true;
    bool sidebarEnabled_ = false;
    float sidebarWidth_ = 220.0f;
    float sidebarScroll_ = 0.0f;  ///< 一覧のスクロール量 (px)
    bool darkTheme_ = true;
    std::string message_;    ///< ステータスバー左側の通知(タイマーで消える。UTF-8)
    std::string hoverText_;  ///< ステータスバー右側(カーソル位置の座標・色。UTF-8)

    // 編集(トリミング・図形・テキスト)の状態
    static constexpr float kDragThresholdPx = 4.0f;  ///< これ未満の右ドラッグは無視(画面px)
    static constexpr size_t kUndoLimit = 10;         ///< undo 履歴の上限
    static constexpr float kHitTolerancePx = 4.0f;   ///< 注釈ヒットテストの許容(画面px)
    static constexpr float kRotationHandleOffsetPx = 20.0f;  ///< 選択枠上辺からハンドルまで
    static constexpr float kRotationHandleRadiusPx = 5.0f;   ///< 回転ハンドルの半径(画面px)
    static constexpr float kRotationHandleHitPx = 9.0f;      ///< 回転ハンドルのヒット判定半径
    static constexpr float kResizeHandleSizePx = 7.0f;  ///< サイズ変更ハンドル(正方形)の一辺
    static constexpr float kResizeHandleHitPx = 8.0f;   ///< 同・ヒット判定の半径
    static constexpr float kAngleSnapDeg = 15.0f;       ///< Shift ドラッグ時のスナップ
    bool selecting_ = false;  ///< 右ドラッグで領域選択中
    Point selStartImage_{};   ///< 選択の始点(画像座標。ズーム中も不変)
    Point selEndImage_{};     ///< 選択の現在点(画像座標。ズーム中も不変)
    Point selStartScreen_{};  ///< ドラッグ量の閾値判定用
    bool edited_ = false;     ///< current_ に未保存の編集(トリミング・注釈)がある

    /// 注釈オブジェクト。current_ には焼き込まず、描画時に重ね、保存/コピー時に合成する
    std::vector<AnnotationSpec> annotations_;
    std::optional<size_t> selected_;  ///< 選択中の注釈 index
    /// @brief 注釈オブジェクトに対する進行中のドラッグ操作。
    enum class ObjectDrag { None, Move, Rotate, Resize };
    ObjectDrag objectDrag_ = ObjectDrag::None;
    bool dragUndoPushed_ = false;  ///< ドラッグ中の undo 記録は最初の変更時の1回だけ
    Point dragStartImage_{};       ///< Move: 掴んだ画像座標
    AnnotationSpec dragOrigSpec_;  ///< ドラッグ開始時の注釈(移動・回転・リサイズの基準)
    float dragStartAngleDeg_ = 0;  ///< Rotate: 開始時のポインタ角度(スクリーン)
    ResizeHandle dragResizeHandle_ = ResizeHandle::BottomRight;  ///< Resize: 掴んだハンドル

    /// @brief undo 1 段分のスナップショット(画像と注釈一覧)。
    struct UndoState {
        std::shared_ptr<DecodedImage> image;      ///< トリミング前の画像
        std::vector<AnnotationSpec> annotations;  ///< そのときの注釈一覧
    };
    std::vector<UndoState> undoStack_;
    uint32_t editColorRGB_ = 0xFF3B30;  ///< 新規注釈の色(0xRRGGBB)
    float editStrokeWidth_ = 3.0f;  ///< 線幅。画面px基準(適用時に 1/zoom で画像座標へ換算)
    float editFontSize_ = 18.0f;    ///< フォントサイズ。画面px基準(同上)
};

} // namespace blinker
