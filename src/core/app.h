#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/animation.h"
#include "core/annotation_edit.h"
#include "core/command.h"
#include "core/config.h"
#include "core/edit_drag_state.h"
#include "core/edit_history.h"
#include "core/edit_style.h"
#include "core/geometry.h"
#include "core/help.h"
#include "core/image_cache.h"
#include "core/image_list.h"
#include "core/image_origin.h"
#include "core/keymap.h"
#include "core/menu.h"
#include "core/mousemap.h"
#include "core/object_drag_state.h"
#include "core/ocr_service.h"
#include "core/pointer_state.h"
#include "core/scan_service.h"
#include "core/sidebar_state.h"
#include "core/sort_order.h"
#include "core/text_edit_state.h"
#include "core/viewport.h"
#include "platform/annotation.h"
#include "platform/clipboard.h"
#include "platform/encoder.h"
#include "platform/file_system.h"
#include "platform/printer.h"
#include "platform/renderer.h"

/**
 * @file app.h
 * @brief アプリ本体の状態機械と、ウィンドウ層へのサービス要求インターフェース。
 */

namespace blinker {

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
     * @brief 取り消せない操作の確認を求める(モーダル。応答されるまで返らない)。
     * @param[in] message 確認したい内容(UTF-8)。
     * @return 続行してよければ true。取りやめなら false。
     * @note 現状の用途は上書き保存の確認だけ(元の画像が失われるため)。
     */
    virtual bool showConfirm(const std::string& message) = 0;

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

    /**
     * @brief アニメーションの次のフレームまでのタイマーを設定する。
     * @param[in] milliseconds 満了までの時間(ミリ秒)。満了で App::onFrameTimer が呼ばれる。
     *                         0 なら動いているタイマーを止める。
     * @note **startTimer とは別のタイマーにすること。** あちらはステータスバーの通知を
     *       消すためのもので、実装は同じ ID を張り直して満了時に止める。共用すると
     *       再生中に通知が出た瞬間にアニメーションが止まる。
     */
    virtual void setFrameTimer(unsigned milliseconds) = 0;

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
     * @param[in] ocr        文字認識の非同期実行。本オブジェクトより長生きすること。
     * @param[in] printer    印刷の実装。
     * @param[in] scan       サブフォルダ走査の非同期実行。本オブジェクトより長生きすること。
     */
    App(IAppHost& host, IFileSystem& fileSystem, ImageCache& cache, IClipboard& clipboard,
        IImageEncoder& encoder, IAnnotationRasterizer& rasterizer, OcrService& ocr,
        IPrinter& printer, ScanService& scan);

    /**
     * @brief blinker.ini の設定を適用する。
     * @param[in] config 適用する設定。キーバインド・背景色・先読み数・
     *                   編集の初期値(色・太さ・起動時のツール)などを反映する。
     */
    void applyConfig(const Config& config);

    /**
     * @brief ダークテーマかどうかを設定する。
     * @param[in] dark true ならダーク配色。ステータスバー・サイドバーの配色に反映される。
     */
    void setDarkTheme(bool dark) { darkTheme_ = dark; }

    /**
     * @brief 操作一覧の存在を知らせる通知をステータスバーに出す(起動時に一度)。
     *
     * 案内の主役は「未割り当てのキーを押したとき」(App::onKey)で、これはその補助。
     * 出す条件は showHelpHint と同じ。
     *
     * @note applyConfig の後、ウィンドウに App を接続してから呼ぶこと
     *       (通知タイマーと再描画要求が host へ飛ぶ)。
     */
    void showStartupHint();

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
     * @brief 垂直ホイール操作を処理する。
     *
     * Mousemap に割り当てがあればそのコマンドを実行し、無ければカーソル位置基準の
     * ズームになる(ズームは Command と等価でないため割り当ての対象外で、
     * 「未割り当てのホイールの既定動作」という位置づけ)。
     *
     * @param[in] wheelNotches 回転量(ノッチ単位)。正で奥へ(既定ではズームイン)。
     * @param[in] screenPos    ポインタ位置。サイドバー上ならズームではなくスクロールする。
     * @param[in] ctrl         Ctrl が押されているか(既定では Ctrl+ホイールが画像の遷移)。
     * @param[in] shift        Shift が押されているか。
     * @param[in] alt          Alt が押されているか。
     * @note サイドバー上では割り当てより一覧のスクロールを優先する(修飾キー付きも同じ)。
     */
    void onWheel(float wheelNotches, Point screenPos, bool ctrl = false, bool shift = false,
                 bool alt = false);

    /**
     * @brief 水平ホイール(チルト)操作を処理する。
     *
     * 既定では左右が前後の画像。垂直ホイールと違い未割り当て時の既定動作は無い。
     *
     * @param[in] wheelNotches 回転量(ノッチ単位)。正で右へ。
     * @param[in] screenPos    ポインタ位置。サイドバー上なら何もしない。
     * @param[in] ctrl         Ctrl が押されているか。
     * @param[in] shift        Shift が押されているか。
     * @param[in] alt          Alt が押されているか。
     * @note サイドバー上では無視する(横スクロールする中身が無く、一覧を読んでいる
     *       最中に画像が切り替わると邪魔になるため)。
     * @note 誤爆しやすい軸なので、垂直ホイールが来たら貯金は捨てる(軸ロック)。
     *       1 段とみなすノッチ数も `[mouse] wheel_horizontal_threshold`(既定 1)で
     *       鈍くできる。
     */
    void onWheelHorizontal(float wheelNotches, Point screenPos, bool ctrl = false,
                           bool shift = false, bool alt = false);

    /**
     * @brief コマンドを割り当てられるマウス操作(中ボタン・サイドボタン等)を処理する。
     *
     * 左右ボタンのクリック・ドラッグはここを通らない(onMouseDown / onMouseUp が
     * mouseRole に従って振り分ける)。
     *
     * @param[in] chord     入力されたマウス操作。
     * @param[in] screenPos ポインタ位置(スクリーン座標)。
     * @return 割り当てがあり実行したら true。未割り当てなら false。
     */
    bool onMouseInput(const MouseChord& chord, Point screenPos);

    /**
     * @brief マウスボタン押下を処理する。
     *
     * 左ボタンはまず注釈オブジェクトを掴めるかを試し(beginObjectGrab)、掴めなければ
     * mouseRole に従う。右ボタンは常に役割どおり(パン / 編集ドラッグ)。
     * サイドバーの項目クリックは UI 部品の操作なので入れ替えの対象外で、
     * 常に左ボタンだけが反応する。
     *
     * @param[in] button    押されたボタン。
     * @param[in] screenPos 押下位置(スクリーン座標)。
     * @return サイドバーのクリック・注釈の選択・編集ドラッグの開始を消費したら true。
     *         false ならパンを開始した(何も掴んでいない)。
     * @note 右ボタンは押下位置も覚える。ドラッグにならずに離されたら onMouseUp が
     *       メニューを開く(メニューは常に右クリック)。
     */
    bool onMouseDown(MouseButton button, Point screenPos);

    /**
     * @brief マウスボタン解放を処理する。
     *
     * 左ボタンなら掴んでいたオブジェクトを離し、編集役のボタンなら現在のツールを適用する
     * (入れ替え時の左ボタンは両方を通るが、掴んでいたなら編集ドラッグは始まっていない)。
     * 右ボタンをドラッグせずに離した場合はメニューを開く。
     *
     * @param[in] button    離されたボタン。
     * @param[in] screenPos 解放位置(スクリーン座標)。メニューの表示位置にもなる。
     * @param[in] shift     Shift が押されているか。押されていれば選択領域を正方形にする
     *                      (直線・矢印・手書きツールは向きを水平 / 垂直 / 45 度へ寄せる)。
     */
    void onMouseUp(MouseButton button, Point screenPos = {}, bool shift = false);

    /**
     * @brief ダブルクリックを処理する(左ボタン)。
     * @param[in] screenPos クリック位置(スクリーン座標)。
     * @param[in] ctrl      Ctrl が押されているか(MouseInput::DoubleClick の割り当て用)。
     * @param[in] shift     Shift が押されているか。
     * @param[in] alt       Alt が押されているか。
     * @return Text 注釈上で編集を開始した、編集中に語を選択した、または
     *         MouseInput::DoubleClick に割り当てられたコマンドを実行したら true。
     * @note テキスト注釈の再編集が優先で、そうならなかったときだけ割り当てを見る。
     */
    bool onDoubleClick(Point screenPos, bool ctrl = false, bool shift = false,
                       bool alt = false);

    /**
     * @brief 画像上でテキストを編集中かを返す。
     * @return 編集中なら true。
     * @note win 層はこれを見てキー入力を文字入力として App へ回す。
     */
    bool isTextEditing() const { return textEdit_.active(); }

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
    bool isComposing() const { return textEdit_.composing(); }

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
     * @brief 指定位置で幅変更用のカーソル(左右の矢印)を出すべきかを返す。
     * @param[in] screenPos ポインタ位置(スクリーン座標)。
     * @return サイドバーの右端を掴める位置か、幅の変更中なら true。
     * @note win 層が WM_SETCURSOR で参照する。変更中も true を返すのは、下限・上限で
     *       止まってポインタが端から離れてもカーソルが戻らないようにするため。
     */
    bool wantsSidebarResizeCursor(Point screenPos) const;

    /**
     * @brief 指定したボタンの役割を返す。
     * @param[in] button 対象のボタン。
     * @return 役割。既定では左が MouseRole::Pan・右が MouseRole::Edit で、
     *         `[mouse] swap_buttons = true` なら入れ替わる。
     * @note メニュー(ツール切り替え・オブジェクト・書式)は入れ替えの対象外で、
     *       常に右ボタンのクリックで開く。注釈オブジェクトを掴む操作も対象外で、
     *       常に左ボタン(役割はそこで何も掴まなかったときの動きを決める)。
     */
    MouseRole mouseRole(MouseButton button) const { return pointer_.role(button); }

    /**
     * @brief 編集ドラッグで実行される現在のツールを返す。
     * @return 現在のツール。
     */
    EditTool currentTool() const { return style_.tool(); }

    /**
     * @brief Shift の押し引きを処理する。
     * @param[in] shift 押されているか。
     * @return ドラッグ中で表示を作り直したら true(呼び出し側はキーをコマンドとして
     *         処理しないこと)。ドラッグ中でなければ false。
     * @note マウスを止めたまま Shift を押し引きしても、正方形・真円のプレビューや
     *       直線・矢印・手書きの向きのスナップが追従するようにするためのもの。
     *       オブジェクトのハンドルを掴んでいる間(端点・回転)も同様に効く。
     */
    bool onShiftChanged(bool shift);

    /**
     * @brief ポインタ移動を処理する。
     * @param[in] screenPos 現在位置(スクリーン座標)。
     * @param[in] shift     Shift が押されているか。回転・リサイズ・正方形のスナップに使う。
     * @note ボタンを押したままの移動もここで処理する(パン、編集ドラッグの選択領域と
     *       プレビュー、注釈の移動・回転・サイズ変更)。それ以外はステータスバーの
     *       座標・色表示を更新する。
     */
    void onMouseMove(Point screenPos, bool shift = false);

    /// @brief ポインタがウィンドウから出たことを通知し、座標・色表示を消す。
    void onMouseLeave();

    /// @brief host のタイマー満了を通知する(ステータスバーの通知メッセージを消す)。
    void onTimer();

    /**
     * @brief アニメーション用タイマーの満了を通知する。
     *
     * 次のフレームへ進めて表示し、次のタイマーを張り直す。末尾に達して繰り返しが
     * 尽きたら再生を止める。
     *
     * @note 再生中でなければ何もしない。UI スレッドで呼ぶこと。
     */
    void onFrameTimer();

    /**
     * @brief デコード完了を通知する。
     * @note UI スレッドで呼ぶこと(ワーカースレッドから直接呼んではならない)。
     */
    void onDecodeCompleted();

    /**
     * @brief 文字認識の完了を通知する。
     *
     * 認識できた文字列をクリップボードへ入れ、結果をステータスバーへ出す。
     * 予約し直された後に届いた古い結果(generation の食い違い)は捨てる。
     *
     * @note 認識中に別の画像へ移っても結果は捨てない。利用者が明示的に頼んだ
     *       認識であり、対象はそのとき表示していた画像で正しいため。
     * @note UI スレッドで呼ぶこと(ワーカースレッドから直接呼んではならない)。
     */
    void onOcrCompleted();

    /**
     * @brief サブフォルダ走査の完了を通知する。
     *
     * 走査結果で一覧を差し替える。表示中の画像は保たれ(パス一致で位置を復元する)、
     * 再デコードも起きない。予約し直された後に届いた古い結果は捨てる。
     *
     * @note UI スレッドで呼ぶこと(ワーカースレッドから直接呼んではならない)。
     */
    void onScanCompleted();

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
     * @brief サイドバーの表示モードを返す。
     * @return ファイル名一覧か操作一覧か。サイドバーが非表示でも最後のモードを返す。
     */
    SidebarMode sidebarMode() const { return sidebar_.mode(); }

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

    /**
     * @brief 画像遷移用オーバーレイ矢印の描画内容を組み立てる。
     * @return ボタンの位置と色(スクリーン座標)。出さない場合は visible が false。
     * @note 使用感が合わなければ廃止しうる表示(`[view] nav_arrows = false` で消せる)。
     *       判定の幾何は core の nav_arrows.h に閉じてある。
     */
    NavArrowsView navArrows() const;

private:
    static constexpr float kPanStepPx = 64.0f;         ///< パンコマンド 1 回の移動量
    static constexpr float kStatusBarHeight = 26.0f;   ///< ステータスバーの高さ
    /// 一覧に載せるファイル数の上限。サブフォルダを再帰で辿ると際限が無くなるため
    static constexpr size_t kMaxListFiles = 100000;

    /**
     * @brief 移動系コマンドの後始末(表示中の画像を一覧の現在位置に合わせ直す)。
     *
     * 貼り付け画像の表示中は、一覧の位置が動かなくてもフォルダ一覧の表示へ戻す。
     * ただし一覧が空のときは戻る先が無いので何もしない(貼り付け画像を保持する)。
     *
     * @param[in] moved ImageList 側で現在位置が動いたか(next/prev/first/last の戻り値)。
     */
    void navigate(bool moved);

    /// @brief 現在位置の画像をキャッシュから取り直し、表示状態を更新する。
    void refreshCurrent();

    /**
     * @brief entries_ を並べ替えて order_ と ImageList を作り直す。
     * @param[in] keep 表示位置にしたいパス。空、または一覧に無ければ先頭を指す。
     */
    void applyListOrder(const std::filesystem::path& keep);

    /**
     * @brief 一覧を作り直して表示へ反映する(並び順の変更・走査の完了)。
     *
     * 表示中の画像はそのまま(番号だけが変わる)で、再デコードも起きない。
     * 表示中の画像が一覧から消えた場合(再帰を切ったなど)だけ現在位置の画像へ切り替える。
     */
    void relist();

    /**
     * @brief サブフォルダの走査を予約する(再帰が有効なときだけ)。
     * @param[in] announce 完了時に件数をステータスバーへ出すか(利用者が切り替えたとき)。
     * @note 起動時にツリー全体を同期で歩くと最初の 1 枚が出るまで固まるため、
     *       直下の同期列挙で表示を確定させたうえでこちらを走らせる。
     */
    void requestScan(bool announce);

    /**
     * @brief 並び順を変更して一覧を作り直す。
     * @param[in] order 新しい並び順。
     */
    void setSortOrder(SortOrder order);

    /**
     * @brief 並び替えキーを選ぶ(Command::SortBy* の共通処理)。
     * @param[in] key 選ぶキー。既に同じキーなら昇順 / 降順を反転する。
     */
    void selectSortKey(SortKey key);

    /**
     * @brief サブフォルダを含めるかを切り替える。
     * @param[in] enabled true で含める。
     * @note 含めない側への切り替えは直下の同期列挙で足りるため走査ワーカーを使わない。
     */
    void setRecursive(bool enabled);

    /**
     * @brief サイドバーの項目に出す文字列を組み立てる。
     * @param[in] index 一覧内のインデックス。
     * @return 通常はファイル名。再帰中は起点フォルダからの相対パス
     *         (ファイル名だけでは別フォルダの同名・連番が区別できないため)。
     */
    std::string sidebarLabel(size_t index) const;

    /**
     * @brief 遅延カラーマネジメントで読み直された画像へ差し替える。
     *
     * ImageCache は色変換の済んだ画像を後から同じパスへ入れ直すので、表示中のものが
     * 古ければ画素だけ入れ替えて再描画する。ズーム・パン・サイドバーの状態は保つ。
     * 編集中(`ImageOrigin::edited`)は呼ばれない — 利用者の編集を捨てないため。
     */
    void adoptRefinedImage();

    // --- フレーム列(アニメーション / 多ページ TIFF / ICO の複数サイズ) ---

    /**
     * @brief キャッシュ側で増えたフレーム構成を拾って表示へ反映する。
     *
     * フレーム構成は「先頭 1 枚だけ」→「調査後」→「全フレーム展開後」と段階的に
     * 差し替わる(ImageCache のコピーオンライト)。完了通知のたびに取り直す。
     */
    void adoptSequence();

    /// @brief フレーム列と再生状態を捨て、タイマーを止める(画像切替・貼り付け時)。
    void resetSequence();

    /**
     * @brief 指定フレームを表示に反映する。
     * @param[in] index 表示するフレーム番号。範囲外なら何もしない。
     * @note 未デコードのページなら読み込みを予約し、前のフレームを表示したまま待つ。
     *       大きさが変わる場合(ICO のサイズ違いなど)だけフィットし直す。
     */
    void showFrame(size_t index);

    /**
     * @brief 再生を開始・停止する。
     * @param[in] play true で再生。アニメーションでない場合は何もしない。
     */
    void setPlaying(bool play);

    /// @brief 再生中なら止めてタイマーを解除する(編集の開始・画像切替)。
    void stopPlayback();

    /// @brief 現在のフレームの表示時間で次のタイマーを張る。
    void scheduleFrameTimer();

    /// @brief 再生 / 一時停止を切り替える (Command::TogglePlay)。
    void executeTogglePlay();

    /**
     * @brief フレーム / ページを手動で送る (Command::NextFrame / PrevFrame)。
     * @param[in] delta +1 で次、-1 で前。
     * @note 端では折り返さず通知を出す(末尾に着いたことが分からなくなるため)。
     *       ファイル間の移動もしない(画像遷移とは完全に分けてある)。
     */
    void stepFrame(int delta);

    /**
     * @brief 表示中のファイルが 2 フレーム以上を持つかを返す。
     * @return フレーム / ページが複数あれば true。
     */
    bool multiFrame() const;

    /**
     * @brief フレームの数え方の呼び名を返す。
     * @return アニメーションなら "フレーム"、多ページなら "ページ"。
     */
    std::string_view frameUnitLabel() const;

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
     * @return サイドバー幅(px)。非表示なら 0。操作一覧モードでは
     *         SidebarState::kHelpWidth 以上。
     */
    float sidebarOffset() const;

    /**
     * @brief サイドバー幅を変更してレイアウトを作り直す。
     * @param[in] width 変更後の幅(px)。下限・上限へクランプされる。
     * @note クランプ後の幅が現在と同じなら何もしない(ドラッグ中の無駄な再描画を避ける)。
     */
    void setSidebarWidth(float width);

    /**
     * @brief 指定位置がサイドバーの右端を掴める帯の中かを返す。
     * @param[in] screenPos 判定する位置(スクリーン座標)。
     * @return 掴める位置なら true。サイドバーが非表示なら常に false。
     * @note 帯は境界をまたぐので、掴める位置はサイドバーの内側と外側の両方にある。
     */
    bool onSidebarResizeEdge(Point screenPos) const;

    /**
     * @brief サイドバーに並ぶ項目数を返す。
     * @return モードに応じた項目数(ファイル数、または操作一覧の行数)。
     */
    size_t sidebarItemCount() const;

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
     * @brief 操作一覧の存在をステータスバーで案内する。
     *
     * blinker.ini の [view] help_hint = false、Command::ToggleHelp が未割り当て、
     * 既に操作一覧を表示中、同じ案内を表示中(キーリピート)のいずれかなら何もしない。
     *
     * @return 案内を出したら true。上記の理由で出さなかったら false。
     */
    bool showHelpHint();

    /**
     * @brief 画像座標を画像の範囲内へ丸める。
     * @param[in] imagePos 丸める前の座標(画像座標)。
     * @return 画像内へクランプされた座標。
     */
    Point clampToImage(Point imagePos) const;

    /**
     * @brief 画像を平行移動して再描画を要求する。
     * @param[in] dx X 方向の移動量(スクリーン px)。
     * @param[in] dy Y 方向の移動量(スクリーン px)。
     * @note パンのドラッグと Command::PanLeft 等のキー操作の共通処理。
     */
    void panBy(float dx, float dy);

    /**
     * @brief 押下位置がビューポート(画像の表示領域)の中かを返す。
     * @param[in] screenPos 判定する位置(スクリーン座標)。
     * @return 画像を表示中で、サイドバーもステータスバーも含まない領域なら true。
     * @note 編集ドラッグとメニューの開始判定に使う(どちらも画像に対する操作なので、
     *       UI 部品の上からは始めない)。
     */
    bool inViewportArea(Point screenPos) const;

    /**
     * @brief サイドバーのファイル名一覧のクリックを処理する。
     * @param[in] screenPos クリック位置(スクリーン座標)。サイドバーの内側であること。
     * @note 操作一覧の表示中は何もしない(クリックできる項目がない)。
     */
    void clickSidebarItem(Point screenPos);

    /**
     * @brief 左ボタンの押下でオブジェクトを掴めるかを試す。
     * @param[in] screenPos 押下位置(スクリーン座標)。
     * @return 注釈のハンドル・本体、または編集中テキストの内側を掴んだら true。
     *         false なら呼び出し元が役割どおりの動き(パン / 編集ドラッグ)を始める。
     * @note 何も掴めなかったときは選択を解除し、テキスト編集中なら確定する
     *       (掴めなかった = 図形の外を押した、ということ)。
     */
    bool beginObjectGrab(Point screenPos);

    /// @brief 掴んでいたオブジェクトを離す(移動・回転・サイズ変更を終了する)。
    void endObjectGrab();

    /**
     * @brief 編集役のボタンの押下を処理する(選択領域の開始)。
     * @param[in] screenPos 開始位置(スクリーン座標)。画像外・サイドバー上は無視する。
     */
    void beginEditDrag(Point screenPos);

    /**
     * @brief 編集ドラッグ中の選択領域(とプレビュー)を更新する。
     * @param[in] screenPos 現在位置(スクリーン座標)。
     * @param[in] shift     Shift が押されているか。押されていれば選択領域を正方形にする
     *                      (直線・矢印・手書きツールは向きを水平 / 垂直 / 45 度へ寄せる)。
     */
    void updateEditDrag(Point screenPos, bool shift);

    /**
     * @brief 編集ドラッグを終了し、現在のツールを選択領域へ適用する。
     * @param[in] screenPos 終了位置(スクリーン座標)。
     * @param[in] shift     Shift が押されているか(updateEditDrag と同じ)。
     * @note 移動量が閾値未満(= 単なるクリック)なら何も作らない。メニューを出すかは
     *       onMouseUp が決める(メニューは常に右クリック)。
     */
    void endEditDrag(Point screenPos, bool shift);

    /**
     * @brief 右クリック(ドラッグなし)のメニューを開く。
     * @param[in] screenPos クリック位置(スクリーン座標)。メニューの表示位置になる。
     * @note 注釈の上ならその注釈を選択してオブジェクトメニュー、そうでなければ
     *       ツール切り替えメニューを出す。
     */
    void showPointerMenu(Point screenPos);

    /**
     * @brief 編集ドラッグ中のポインタ位置を選択領域の終点(画像座標)へ変換する。
     * @param[in] screenPos ポインタ位置(スクリーン座標)。
     * @param[in] shift     Shift が押されているか。
     * @return 画像内へクランプした終点。shift のときは、直線・矢印・手書きなら水平・
     *         垂直・45 度へ、正方形にできるツールなら選択領域が正方形になる位置へ寄せたもの。
     * @note 手書きの起点は直線アンカー(EditDragState)が指す点なので、先に
     *       updatePenStraightAnchor を呼んでおくこと。
     */
    Point dragEndImage(Point screenPos, bool shift) const;

    /**
     * @brief 手書きの直線アンカーを Shift とツールの状態に合わせる。
     * @param[in] shift Shift が押されているか。
     * @note dragEndImage が参照するので、終点を求める前に呼ぶこと。
     */
    void updatePenStraightAnchor(bool shift);

    /**
     * @brief 手書きの軌跡を編集ドラッグの終点まで伸ばす。
     * @param[in] minDistancePx 直前の点との最小距離(画像座標)。これ未満の動きは捨てる。
     * @note 手書きツール以外では何もしない。直線アンカーがある間は、アンカーから
     *       終点までのまっすぐな 1 本で置き換える。
     */
    void extendPenPoints(float minDistancePx);

    /**
     * @brief 使えるフォントかを答える述語を返す(メニューの組み立てへ渡す)。
     * @return ラスタライザへ問い合わせる述語。
     * @note 返した述語は this を捕まえているので、App より長生きさせないこと。
     */
    FontAvailableFn fontAvailable() const;

    /**
     * @brief 表示中の画像の大きさをメニュー用に返す。
     * @return 画像の幅と高さ。表示する画像が無ければ std::nullopt。
     */
    std::optional<MenuImageSize> menuImageSize() const;

    /**
     * @brief ツール切り替えメニューを表示し、選ばれた項目を適用する。
     * @param[in] screenPos メニューの表示位置(スクリーン座標)。
     * @note 設定系(太さ・色など)を選んだ場合はメニューを再表示し、続けて選べるようにする。
     */
    void showToolMenu(Point screenPos);

    /**
     * @brief ツール切り替えメニューで選ばれた項目を適用する。
     * @param[in] entry 選択された末端項目。
     * @return メニューを閉じるなら true。設定系で再表示するなら false。
     */
    bool applyEditChoice(const EditMenuEntry& entry);

    /**
     * @brief リサイズのプリセットメニューを表示し、選ばれた大きさへ変換する。
     * @param[in] screenPos メニューの表示位置(スクリーン座標)。
     */
    void showResizeMenu(Point screenPos);

    /**
     * @brief 表示中の画像をリサイズする(トリミングと同じ破壊的編集)。
     *
     * 注釈オブジェクトは焼き込まず、同じ倍率で座標を追従させる(scaleAnnotation)。
     * 保存は既存の経路のままで、取り込み時に縮小された画像は上書き保存の拒否を保つ。
     *
     * @param[in] width  変換後の幅(ピクセル)。
     * @param[in] height 変換後の高さ(ピクセル)。
     */
    void applyResize(uint32_t width, uint32_t height);

    /**
     * @brief サイドバーのメニューを表示し、選ばれた項目を適用する。
     * @param[in] screenPos メニューの表示位置(スクリーン座標)。
     * @note 並び替えと再帰は既定のキーを持たないので、ここが主な入口になる。
     */
    void showSidebarMenu(Point screenPos);

    /**
     * @brief 現在のツールを切り替え、表示を更新する。
     * @param[in] tool 切り替え先のツール。
     * @note Crop 以外を選ぶと、トリミング実行後に戻る図形ツールも更新される
     *       (EditStyle::setTool)。
     */
    void setTool(EditTool tool);

    /// @brief 現在のツールを選択領域へ適用する(編集ドラッグの確定時)。
    void applyCurrentTool();

    /**
     * @brief 次に置く連番マーカーの番号を求める。
     * @return 既にある連番マーカーの最大値 + 1(無ければ 1)。
     * @note 状態として持たず毎回数えることで、undo・削除のあとも番号が詰まる。
     */
    int nextMarkerNumber() const;

    /**
     * @brief 選択領域と現在の設定から新しい注釈を組み立てる。
     * @param[in] kind 組み立てる注釈の種別。
     * @return 画像座標の注釈。線幅・文字サイズはズームで画像座標へ換算済み。
     * @note 追加前のプレビュー描画にも同じものを使う(見た目が結果と一致する)。
     */
    AnnotationSpec makeAnnotationSpec(AnnotationSpec::Kind kind) const;

    /// @brief 編集ドラッグ中のプレビュー注釈を選択領域から作り直す。
    void updatePreview();

    /**
     * @brief 編集ドラッグ中に実物の図形をプレビューとして描くかを返す。
     * @return 描くなら true。トリミングとテキストは形が定まらないため false
     *         (代わりにラバーバンドを出す)。
     */
    bool previewVisible() const;

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

    /**
     * @brief 選択領域で現在の画像をトリミングする。
     * @return 切り出せたら true。有効領域が残らなければ false(画像は変わらない)。
     */
    bool applyCrop();

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
    void beginTextEdit(size_t index, EditSnapshot before, bool created);

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

    /// @brief キャレット位置を host へ通知し(IME の位置合わせ)、点滅を表示相に戻す。
    void notifyCaretMoved();

    /// @brief 最初の変更時に 1 回だけ編集前のスナップショットを undo へ積む。
    void pushTextEditUndoOnce();

    /**
     * @brief 編集中のテキスト内で、画像座標に対応する文字位置を求める。
     * @param[in] imagePos 対象の位置(画像座標)。回転は内部で打ち消す。
     * @return テキスト内のバイト位置(UTF-8)。
     * @pre TextEditState::active() が true であること。
     */
    size_t textOffsetAt(Point imagePos) const;

    /**
     * @brief キャレットを 1 行上下へ移動する。
     * @param[in] down            true で下、false で上。
     * @param[in] extendSelection true なら選択を広げる。
     * @note 折り返し位置はラスタライザの計測に従う(論理行ではなく表示行で動く)。
     */
    void moveCaretVertical(bool down, bool extendSelection);

    /**
     * @brief 選択中の Text 注釈全体の太字を切り替える(Ctrl+B)。
     *
     * 全体が太字なら解除、そうでなければ全体を太字にする(編集中の Ctrl+B と同じ規則)。
     * 字幅が変わるため実測境界も測り直す。
     *
     * @return 切り替えた(= キーを消費した)なら true。Text 注釈を選択していない、
     *         または内容が空なら false(呼び出し側は通常のコマンドとして処理する)。
     */
    bool toggleSelectedTextBold();

    /// @brief 選択中の注釈オブジェクトを削除する。
    void deleteSelectedAnnotation();

    /**
     * @brief 注釈と表示回転を焼き込んだ保存・コピー用の画像を作る。
     * @return 焼き込んだ画像。注釈も回転もなければ current_ をそのまま返す。
     * @note 回転は Viewport が持つ表示状態で current_ のピクセルには入っていないため、
     *       外へ出す(保存・コピー)ときにここで反映する。
     */
    std::shared_ptr<DecodedImage> compositeImage() const;

    /**
     * @brief 選択中の注釈オブジェクト 1 件だけをラスタライズする(コピー用)。
     *
     * 下地は含めず、オブジェクトのバウンディングボックス分の画像を返す。
     * 覆われていない部分は透過のままなので、透過を解さない貼り付け先では
     * IClipboard 側で白と合成される。
     *
     * @return オブジェクトの画像。何も選んでいない、またはラスタライズに
     *         失敗したときは nullptr。
     */
    std::shared_ptr<DecodedImage> selectedObjectImage() const;

    /**
     * @brief 画像をクリップボードへコピーする(Command::CopyImage)。
     *
     * オブジェクトを選択している間は、そのオブジェクトだけをコピーする。
     * 何も選んでいなければ compositeImage()(注釈と表示回転を焼き込んだもの)。
     */
    void executeCopyImage();

    /**
     * @brief クリップボードの画像を表示中の画像として開く(Command::PasteImage)。
     *
     * 編集中の内容は捨てられ、一覧の位置から切り離された「貼り付け画像」になる
     * (次/前へ移動するとフォルダの一覧へ戻る)。
     *
     * @return 貼り付けたら true。クリップボードに画像が無ければ false
     *         (その旨をステータスバーへ出したうえで何も変えない)。
     */
    bool executePasteImage();

    /**
     * @brief クリップボードの画像を注釈オブジェクトとして貼る(Command::PasteObject)。
     *
     * 表示中の画像は変えず、Kind::Image の注釈として上へ重ねる(移動・回転・
     * リサイズ・削除・undo は他の注釈と同じ。保存・コピー時に焼き込まれる)。
     * 下地が無いとき、および注釈を扱えない環境(SDL バックエンド)では
     * executePasteImage へ回す ―― 見えないオブジェクトを作らないため。
     */
    void executePasteObject();

    /**
     * @brief 表示中の画像を元のファイルへ上書き保存する(Command::SaveImage)。
     *
     * 上書き先が無い(貼り付け画像・一覧が空)ときは名前を付けて保存へ回す。
     * 元の画像が失われるため、`[save] confirm_overwrite` が真なら
     * IAppHost::showConfirm で確認を取る。
     */
    void executeSaveOverwrite();

    /// @brief 保存先を選んで保存する(Command::SaveImageAs)。
    void executeSaveAs();

    /**
     * @brief 表示中の画像を印刷する(Command::PrintImage)。
     *
     * 印刷するのは保存・コピーと同じ compositeImage()(注釈と表示回転を焼き込んだもの)。
     * 紙は白なので、半透明を含む画像は白へ焼き込んでから渡す。
     * 結果はステータスバーへ出す(利用者が取りやめたときは何も出さない)。
     */
    void executePrint();

    /**
     * @brief compositeImage をファイルへ書き出し、結果をステータスバーへ出す。
     * @param[in] path        保存先のパス。
     * @param[in] isOverwrite 表示中のファイル自身への上書きなら true。成功時に
     *                        未保存マークを消し、キャッシュを無効化する。
     * @pre current_ != nullptr
     */
    void saveImageTo(const std::filesystem::path& path, bool isOverwrite);

    /// @brief 未保存の編集ありとして記録し、タイトルを更新する。
    void markEdited();

    /// @brief 現在の画像と注釈一覧を undo 履歴へ積む。
    void pushUndo();

    /**
     * @brief 与えられたスナップショットを undo 履歴へ積む。
     * @param[in] state 積むスナップショット。所有権を受け取る。
     */
    void pushUndoState(EditSnapshot state);

    /// @brief ドラッグ(移動・回転)の最初の変更時に 1 回だけ undo 履歴へ積む。
    void pushDragUndoOnce();

    /// @brief undo 履歴から 1 つ戻す(戻す前の状態は redo 履歴へ積む)。
    void executeUndo();

    /// @brief redo 履歴から 1 つやり直す(やり直す前の状態は undo 履歴へ積む)。
    void executeRedo();

    /**
     * @brief 履歴から取り出したスナップショットを表示状態へ復元する。
     * @param[in] state EditHistory::undo / redo の戻り値。
     * @return 復元したら true。std::nullopt なら false(何もしない)。
     */
    bool restoreFrom(std::optional<EditSnapshot> state);

    /// @brief 画像切替時に編集を破棄する(編集があれば通知を出す)。
    void discardEdits();

    /**
     * @brief 文字認識を予約する。
     *
     * 透明部分を持つ画像は白へ焼き込んでから渡す(認識器はアルファを見ないため、
     * 事前乗算のままだと透明部分が黒になって文字が沈む)。
     *
     * @param[in] image 認識対象の画像。nullptr なら通知だけ出して何もしない。
     * @note 予約するだけで、結果は onOcrCompleted で受け取る。
     */
    void requestOcr(const std::shared_ptr<DecodedImage>& image);

    /**
     * @brief 選択領域の文字認識を予約する(EditTool::Ocr の編集ドラッグ確定時)。
     * @return 予約できたら true。領域が画像の外で切り出せなければ false。
     */
    bool applyOcrSelection();

    /**
     * @brief ダブルクリックで Text 注釈の編集に入れるかを試す。
     * @param[in] screenPos クリック位置(スクリーン座標)。
     * @return 編集を開始した、または編集中に語を選択したら true。
     */
    bool beginTextEditByDoubleClick(Point screenPos);

    /**
     * @brief ホイールの回転を割り当てられたコマンドとして実行する。
     * @param[in] notches    回転量(ノッチ単位)。
     * @param[in] horizontal 水平ホイールなら true。
     * @param[in] ctrl       Ctrl が押されているか。
     * @param[in] shift      Shift が押されているか。
     * @param[in] alt        Alt が押されているか。
     * @return 割り当てがあれば true(1 段に達しておらず何も実行しなかった場合も
     *         含む。呼び出し側はズームへ落とさないこと)。未割り当てなら false。
     * @note 1 段とみなすノッチ数は垂直が 1.0 固定、水平が
     *       PointerState::horizontalThreshold()。
     */
    bool wheelCommand(float notches, bool horizontal, bool ctrl, bool shift, bool alt);

    // --- オーバーレイ矢印(廃止しうる表示。消すときはこの 3 つと navArrows / メンバ) ---

    /**
     * @brief オーバーレイ矢印の現在の状態を求める(スクリーン座標)。
     * @return 左右のボタンの位置と表示状態。出さない状況では visible が false。
     * @note 出さない状況: `[view] nav_arrows = false`、画像なし、ポインタがウィンドウ外か
     *       ビューポート外、ドラッグ中(パン・編集・オブジェクト・幅変更)、
     *       テキスト編集中(端のボタンを押して編集が消えるのを防ぐ)。
     */
    NavArrowsState navArrowsGeometry() const;

    /**
     * @brief クリック位置がオーバーレイ矢印なら画像を送る。
     * @param[in] screenPos クリック位置(スクリーン座標)。
     * @return ボタンに当たって遷移したら true。
     */
    bool clickNavArrow(Point screenPos);

    /**
     * @brief ポインタ移動でオーバーレイ矢印の表示が変わったかを調べ、状態を更新する。
     * @return 表示・ホバーが変わって再描画が必要なら true。
     */
    bool updateNavArrowHover();

    IAppHost& host_;
    IFileSystem& fileSystem_;
    ImageCache& cache_;
    IClipboard& clipboard_;
    IImageEncoder& encoder_;
    IAnnotationRasterizer& rasterizer_;
    OcrService& ocr_;
    IPrinter& printer_;
    ScanService& scan_;
    /// 待っている認識の generation。0 なら認識中でない。古い結果を捨てるのに使う
    uint64_t ocrGeneration_ = 0;
    /// 待っているサブフォルダ走査の generation。0 なら走査中でない(同上)
    uint64_t scanGeneration_ = 0;
    bool scanAnnounce_ = false;  ///< 走査の完了時に件数をステータスバーへ出すか
    Keymap keymap_ = Keymap::defaults();
    Mousemap mousemap_ = Mousemap::defaults();
    ImageList list_;
    /// 一覧の起点フォルダ。並び順の変更・再帰の切り替えで列挙し直すのに使う
    std::filesystem::path listRoot_;
    /// 列挙結果。IFileSystem の契約どおり名前昇順のまま持ち、並べ替えは order_ で表す
    std::vector<FileEntry> entries_;
    /// 表示順(entries_ への添字)。list_ と同じ並びで、サイドバーの相対パス表示に使う
    std::vector<size_t> order_;
    SortOrder sortOrder_;          ///< 一覧の並び順([view] sort / sort_descending)
    bool recursive_ = false;       ///< サブフォルダを含めるか([view] recursive)
    bool listTruncated_ = false;   ///< 列挙が kMaxListFiles で打ち切られたか
    Viewport viewport_;
    std::shared_ptr<DecodedImage> current_;
    /// 表示中ファイルのフレーム構成。current_ は常にこの中の 1 フレームを指す
    /// (保存・コピー・印刷・文字認識・描画はすべて current_ を見ているので影響を受けない)
    std::shared_ptr<const ImageSequence> sequence_;
    PlaybackState playback_;             ///< 表示中のフレーム番号と再生状態
    AnimationOptions animationOptions_;  ///< 再生の設定([animation] セクション)
    /// current_ の出どころ(どのパスの画像か・貼り付け画像か・読み込みに失敗したか)と、
    /// 未保存の編集があるか。画素そのもの (current_) と一覧との突き合わせは App 側
    ImageOrigin origin_;
    uint32_t backgroundRGB_ = 0x202020;
    EncodeOptions encodeOptions_;   ///< 保存時のエンコード設定([save] jpeg_quality)
    PrintOptions printOptions_;     ///< 印刷時の余白・自動回転([print] セクション)
    bool confirmOverwrite_ = true;  ///< 上書き保存の前に確認を取るか([save] confirm_overwrite)
    int prefetchRadius_ = 2;
    SizeF clientSize_{};  ///< クライアント領域全体(サイドバー + ビューポート + ステータスバー)
    bool statusBarEnabled_ = true;
    /// サイドバーの可視・モード・幅・スクロール量と、右端を掴む幅変更ドラッグの状態。
    /// 一覧の中身は App 側(list_ と helpLines_)にあり、SidebarState は知らない
    SidebarState sidebar_;
    /// 操作一覧の表示行。毎フレーム組み立てずに Command::ToggleHelp で開いたときだけ作る
    std::vector<HelpLine> helpLines_;
    bool helpHintEnabled_ = true;  ///< 操作一覧の存在をステータスバーで案内するか
    bool darkTheme_ = true;
    std::string message_;    ///< ステータスバー左側の通知(タイマーで消える。UTF-8)
    std::string hoverText_;  ///< ステータスバー右側(カーソル位置の座標・色。UTF-8)

    // マウス操作の状態
    /// 左右の役割(パンと編集)・ポインタ位置・パン中かどうか・右クリックの押下位置・
    /// ホイールの貯金。画像に触る編集ドラッグ (drag_) とオブジェクト操作
    /// (objectDrag_) は下の「編集の状態」にあり、PointerState は知らない
    PointerState pointer_;
    /// オーバーレイ矢印を出すか(`[view] nav_arrows`)。使用感が合わなければ false にできる
    bool navArrowsEnabled_ = true;
    /// 直前に描いた矢印の状態。ポインタ移動で再描画が必要かの判定にだけ使う
    NavArrowsState navArrowsShown_;

    // 編集(トリミング・図形・テキスト)の状態
    static constexpr float kHitTolerancePx = 4.0f;   ///< 注釈ヒットテストの許容(画面px)
    static constexpr float kRotationHandleOffsetPx = 20.0f;  ///< 選択枠上辺からハンドルまで
    static constexpr float kRotationHandleRadiusPx = 5.0f;   ///< 回転ハンドルの半径(画面px)
    static constexpr float kRotationHandleHitPx = 9.0f;      ///< 回転ハンドルのヒット判定半径
    static constexpr float kResizeHandleSizePx = 7.0f;  ///< サイズ変更ハンドル(正方形)の一辺
    static constexpr float kResizeHandleHitPx = 8.0f;   ///< 同・ヒット判定の半径
    static constexpr float kAngleSnapDeg = 15.0f;       ///< Shift ドラッグ時のスナップ
    /// 編集ドラッグの進行状態。始点・終点(画像座標)と手書きの軌跡。
    /// 画像座標への変換・クランプ・Shift での方向合わせ・ツールの適用は App 側
    EditDragState drag_;
    /// 選択中のツールと、新規注釈へ写す見た目(色・線幅・文字・塗り・枠線)。
    /// 何をどこへ描くか(適用先の種別・位置)は App 側
    EditStyle style_;
    /// 編集ドラッグ中のプレビュー。図形ツールのときだけ有効(Crop/Text はラバーバンドを出す)
    AnnotationSpec previewSpec_;
    /// 軌跡へ点を足す最小間隔(画面px)。これ未満の動きは無視して点数を抑える
    static constexpr float kPenMinDistancePx = 2.0f;

    /// 注釈オブジェクト。current_ には焼き込まず、描画時に重ね、保存/コピー時に合成する
    std::vector<AnnotationSpec> annotations_;
    std::optional<size_t> selected_;  ///< 選択中の注釈 index
    /// 注釈オブジェクトを掴んでいる間の状態(移動・回転・サイズ変更と、その基準値)。
    /// 対象は selected_ が指す。ハンドルのヒット判定と実際の変形は App 側
    ObjectDragState objectDrag_;

    // Text 注釈のインプレース編集(画像上で直接入力する状態)
    /// 編集中かどうか・対象の注釈 index・編集バッファ・キャレットの点滅・
    /// ドラッグ選択・書式メニューの押下・IME の変換中文字列。
    /// 対象の注釈そのもの(annotations_)と、枠の実測・キャレット位置の通知は App 側
    TextEditState textEdit_;

    /// 取り消し・やり直しの履歴。ドラッグ中・テキスト編集中の「最初の 1 回だけ積む」
    /// 判定もここが持つ(旗と積み先が食い違わないようにするため)
    EditHistory history_;
};

} // namespace blinker
