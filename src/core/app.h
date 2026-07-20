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
#include "core/text_edit.h"
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
     * @brief 画像上でのテキスト編集の開始・終了・キャレット移動を通知する。
     *
     * win 層は IME の有効・無効の切り替え、変換ウィンドウの位置合わせ、
     * キャレット点滅タイマー(満了で App::onCaretBlink を呼ぶ)に使う。
     *
     * @param[in] active         編集中なら true、終了したら false。
     * @param[in] caretScreenPos キャレット上端の位置(スクリーン座標)。false のときは無意味。
     * @param[in] caretHeightPx  キャレットの高さ(画面 px)。IME 変換ウィンドウの
     *                           フォントサイズに使う。false のときは無意味。
     */
    virtual void setTextEditing(bool active, Point caretScreenPos, float caretHeightPx) = 0;

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
     * @return Text 注釈上で編集を開始した、または編集中に語を選択したら true。
     */
    bool onDoubleClick(Point screenPos);

    /**
     * @brief 画像上でテキストを編集中かを返す。
     * @return 編集中なら true。
     * @note win 層はこれを見てキー入力を文字入力として App へ回す。
     */
    bool isTextEditing() const { return textEditing_; }

    /**
     * @brief 編集中のテキストへ文字列を挿入する(文字キー入力・IME 確定・貼り付け)。
     * @param[in] utf8 挿入する文字列(UTF-8。改行 LF 可)。
     * @note 編集中でなければ何もしない。選択範囲があれば置き換える。
     */
    void insertText(const std::string& utf8);

    /**
     * @brief キャレット点滅タイマーの満了を通知する。
     * @note 編集中でなければ何もしない。
     */
    void onCaretBlink();

    /**
     * @brief IME の変換を開始する。
     * @note 選択範囲があれば削除してキャレットを 1 点にする(変換は選択を置き換える)。
     *       編集中でなければ何もしない。
     */
    void beginComposition();

    /**
     * @brief IME の変換中文字列を設定する(テキストボックス内へインライン表示する)。
     *
     * 変換中文字列はキャレット位置に挿入した形で描画され、確定するまで
     * 編集内容(TextEditBuffer)には入らない。
     *
     * @param[in] utf8        変換中文字列(UTF-8)。空なら変換なしとして扱う。
     * @param[in] caretBytes  変換中文字列内のキャレット位置(先頭からのバイト数)。
     * @param[in] targetBegin 変換対象の節の開始位置(同上)。無ければ 0。
     * @param[in] targetEnd   変換対象の節の終了位置(同上)。無ければ 0。
     * @note targetBegin/End の範囲は太い下線、それ以外の変換中文字列は細い下線で描く。
     */
    void setComposition(const std::string& utf8, size_t caretBytes, size_t targetBegin,
                        size_t targetEnd);

    /// @brief 変換中文字列を破棄する(変換のキャンセル・確定時)。
    void clearComposition();

    /**
     * @brief IME で変換中かを返す。
     * @return 変換中文字列があれば true。
     */
    bool isComposing() const { return !composition_.empty(); }

    /// @brief 編集中のテキストを確定して編集を終了する(内容が空なら注釈を削除する)。
    void commitTextEdit();

    /**
     * @brief 指定位置でテキスト編集用のカーソル(I ビーム)を出すべきかを返す。
     * @param[in] screenPos ポインタ位置(スクリーン座標)。
     * @return 編集中で、かつ編集中のテキストボックスの内側なら true。
     * @note win 層が WM_SETCURSOR で参照する。「クリックすれば文字を入力できる」
     *       ことを示すためのもので、編集していないテキスト注釈の上では false。
     */
    bool wantsTextCursor(Point screenPos) const;

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

    /// @brief undo 1 段分のスナップショット(画像と注釈一覧)。
    struct UndoState {
        std::shared_ptr<DecodedImage> image;      ///< トリミング前の画像
        std::vector<AnnotationSpec> annotations;  ///< そのときの注釈一覧
    };

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

    /**
     * @brief 編集中テキストの選択部分に対する書式メニューを表示し、選ばれた書式を適用する。
     * @param[in] screenPos メニューの表示位置(スクリーン座標)。
     * @note 編集中で選択範囲があるときだけ意味を持つ(それ以外は何もしない)。
     */
    void showTextStyleMenu(Point screenPos);

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
     * @brief Text 注釈の幅を保ったまま高さだけ内容に合わせる(編集中の枠追従用)。
     * @param[in,out] spec 対象の注釈。成功時に p2.y が更新される。
     * @return 実測できたら true。ラスタライズに失敗したら false。
     * @note 編集中に幅まで詰めると入力のたびに折り返しが変わってしまうため、
     *       幅は編集開始時のまま固定し、確定時に measureTextExtent で詰める。
     */
    bool measureTextHeight(AnnotationSpec& spec);

    /**
     * @brief Text 注釈のインプレース編集を開始する。
     * @param[in] index   編集する注釈の index。
     * @param[in] before  最初の変更時に undo へ積むスナップショット(編集前の状態)。
     * @param[in] created 新規作成直後なら true。空のまま終了したら注釈ごと削除する。
     */
    void beginTextEdit(size_t index, UndoState before, bool created);

    /// @brief 編集を破棄して終了する(新規作成中なら注釈も削除する)。
    void cancelTextEdit();

    /**
     * @brief 編集中のキー入力を処理する。
     * @param[in] chord 入力されたキー。
     * @return 編集操作として処理したら true。
     * @note 編集中は未処理のキーも true を返して握りつぶす(移動コマンド等の暴発防止)。
     */
    bool handleTextEditKey(const KeyChord& chord);

    /// @brief 編集を記録(undo・編集済みマーク)したうえで注釈へ書き戻す。
    void applyTextEditChange();

    /**
     * @brief 表示用テキストを注釈へ書き戻し、枠の高さと再描画を更新する。
     * @note undo 記録も編集済みマークも行わない。変換中文字列の更新のように
     *       確定していない表示の変化に使う。
     */
    void refreshTextEditSpec();

    /**
     * @brief 描画に使うテキストを組み立てる。
     * @return 編集中の文字列に、変換中文字列をキャレット位置へ挿入したもの(UTF-8)。
     */
    std::string textEditDisplayText() const;

    /**
     * @brief 描画に使う部分書式を組み立てる。
     * @return 編集中の部分書式を、変換中文字列の挿入分だけずらしたもの。
     * @note 変換中文字列そのものは直前の文字の書式を継ぐ(adjustTextStyles と同じ規則)。
     */
    std::vector<TextStyleRun> textEditDisplayStyles() const;

    /**
     * @brief 表示用テキスト内でのキャレット位置を返す。
     * @return バイト位置。変換中は変換中文字列内のキャレットを加えた位置。
     */
    size_t textEditCaretOffset() const;

    /// @brief 変換中文字列の状態を消す。
    void resetComposition();

    /// @brief キャレット位置を host へ通知し(IME の位置合わせ)、点滅を表示相に戻す。
    void notifyCaretMoved();

    /// @brief 最初の変更時に 1 回だけ編集前のスナップショットを undo へ積む。
    void pushTextEditUndoOnce();

    /**
     * @brief 編集中のテキスト内で、画像座標に対応する文字位置を求める。
     * @param[in] imagePos 対象の位置(画像座標)。回転は内部で打ち消す。
     * @return テキスト内のバイト位置(UTF-8)。
     * @pre textEditing_ が true であること。
     */
    size_t textOffsetAt(Point imagePos) const;

    /**
     * @brief キャレットを 1 行上下へ移動する。
     * @param[in] down            true で下、false で上。
     * @param[in] extendSelection true なら選択を広げる。
     * @note 折り返し位置はラスタライザの計測に従う(論理行ではなく表示行で動く)。
     */
    void moveCaretVertical(bool down, bool extendSelection);

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

    /**
     * @brief 与えられたスナップショットを undo 履歴へ積む。
     * @param[in] state 積むスナップショット。所有権を受け取る。
     */
    void pushUndoState(UndoState state);

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

    // Text 注釈のインプレース編集(画像上で直接入力する状態)
    bool textEditing_ = false;      ///< 編集中か
    size_t textEditIndex_ = 0;      ///< 編集中の注釈 index(annotations_ 内)
    TextEditBuffer textBuffer_;     ///< 編集中の文字列・キャレット・選択範囲
    bool textEditCreated_ = false;  ///< 新規作成中(空のまま終了したら注釈を消す)
    bool textEditCaretOn_ = true;   ///< キャレット点滅の表示相
    bool textEditMouseSelect_ = false;  ///< 左ドラッグで選択範囲を広げている最中
    /// 右ボタンを選択範囲の上で押した。離した位置で書式メニューを出す(編集は確定しない)
    bool textStyleMenuPending_ = false;
    bool textUndoPushed_ = false;   ///< 編集中の undo 記録は最初の変更時の1回だけ
    UndoState textUndoState_;       ///< 上記で積む編集前のスナップショット
    /// IME の変換中文字列(UTF-8)。確定するまで textBuffer_ には入れず、表示にだけ混ぜる
    std::string composition_;
    size_t compositionCaret_ = 0;        ///< 変換中文字列内のキャレット(バイト位置)
    size_t compositionTargetBegin_ = 0;  ///< 変換対象の節の開始(同上)
    size_t compositionTargetEnd_ = 0;    ///< 変換対象の節の終了(同上)

    std::vector<UndoState> undoStack_;
    uint32_t editColorRGB_ = 0xFF3B30;  ///< 新規注釈の色(0xRRGGBB)
    float editStrokeWidth_ = 3.0f;  ///< 線幅。画面px基準(適用時に 1/zoom で画像座標へ換算)
    float editFontSize_ = 18.0f;    ///< フォントサイズ。画面px基準(同上)
};

} // namespace blinker
