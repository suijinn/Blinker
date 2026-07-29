#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "core/text_edit.h"
#include "core/text_style.h"

/**
 * @file text_edit_state.h
 * @brief 画像上での Text 注釈のインプレース編集の進行状態(IME の変換中文字列を含む)。
 *
 * OS ヘッダに依存しない(単体テスト対象)。**注釈も画像も窓も知らない** ―
 * 編集対象は index で指すだけで、注釈への書き戻し・枠の実測・キャレット位置の
 * 通知は App が行う(どれもラスタライザの計測を要するため)。
 */

namespace blinker {

/**
 * @brief Text 注釈をその場で編集している間の状態。
 *
 * 編集中の文字列そのものは TextEditBuffer が持ち、この型はその周りの
 * 「編集セッション」の状態 ― 編集中か・どの注釈か・新規作成中か・キャレットの
 * 点滅相・ドラッグで範囲選択中か・書式メニューの押下・IME の変換中文字列 ― を持つ。
 *
 * @note UI スレッド専用(App と同じ)。スレッド安全ではない。
 */
class TextEditState {
public:
    /**
     * @brief 編集を開始する。
     * @param[in] index   編集する注釈の index(App の annotations_ 内)。
     * @param[in] created 新規作成直後なら true。
     * @param[in] text    編集の初期内容(UTF-8)。注釈の現在の文字列を渡す。
     * @param[in] styles  初期の部分書式。続きの入力が直前の書式を継ぐように引き継ぐ。
     * @note キャレットは末尾に置かれ、点滅は表示相から始まる。変換中文字列と
     *       書式メニューの押下は捨てられる。
     */
    void begin(size_t index, bool created, std::string text, std::vector<TextStyleRun> styles);

    /**
     * @brief 編集を終える(確定・取り消し・破棄に共通)。
     * @note **バッファ・index・新規作成中の旗は残す。** 確定処理が編集内容を
     *       注釈へ書き戻すのは編集終了後だから。次の begin() で作り直される。
     */
    void end();

    /**
     * @brief 編集中かを返す。
     * @return 編集中なら true。
     */
    bool active() const { return active_; }

    /**
     * @brief 編集中の注釈の index を返す。
     * @return 開始時に渡された index。編集していないときは最後の値。
     */
    size_t index() const { return index_; }

    /**
     * @brief 新規作成中かを返す。
     * @return 新規作成直後の編集なら true(空のまま終わったら注釈ごと消す判断に使う)。
     */
    bool created() const { return created_; }

    /**
     * @brief 編集バッファを返す。
     * @return 編集中の文字列・キャレット・選択範囲・部分書式。
     * @note 文字の挿入・削除・キャレット移動・書式の変更はこの参照経由で行う
     *       (転送メソッドを並べても TextEditBuffer の API をなぞるだけのため)。
     */
    TextEditBuffer& buffer() { return buffer_; }

    /**
     * @brief 編集バッファを返す(const 版)。
     * @return 編集中の文字列・キャレット・選択範囲・部分書式。
     */
    const TextEditBuffer& buffer() const { return buffer_; }

    /**
     * @brief キャレット点滅の表示相を返す。
     * @return 表示相なら true。
     */
    bool caretOn() const { return caretOn_; }

    /// @brief キャレットを表示相へ戻す(キャレットが動いた直後は必ず見えているように)。
    void showCaret() { caretOn_ = true; }

    /// @brief キャレット点滅の表示相を反転する(点滅タイマーの満了)。
    void blinkCaret() { caretOn_ = !caretOn_; }

    /**
     * @brief ドラッグで選択範囲を広げている最中かを返す。
     * @return 範囲選択中なら true。
     */
    bool mouseSelecting() const { return mouseSelecting_; }

    /// @brief ドラッグでの範囲選択を始める(枠内での押下)。
    void beginMouseSelect() { mouseSelecting_ = true; }

    /// @brief ドラッグでの範囲選択を終える(ボタンの解放)。
    void endMouseSelect() { mouseSelecting_ = false; }

    /// @brief 書式メニューの押下を覚える(メニューはボタンを離した位置で出す)。
    void pressStyleMenu() { styleMenuPending_ = true; }

    /**
     * @brief 覚えてある書式メニューの押下を消費する。
     * @return 押下を覚えていたら true(呼び出し側がメニューを開く)。
     * @note 消費するので 2 回目は false。押下を覚えるのは選択範囲の上での右クリックに
     *       限られ、離した位置でメニューを出す(編集は確定しない)。
     */
    bool consumeStyleMenu();

    /**
     * @brief IME で変換中かを返す。
     * @return 変換中文字列があれば true。
     */
    bool composing() const { return !composition_.empty(); }

    /**
     * @brief 変換中文字列を返す。
     * @return 変換中文字列(UTF-8)。変換中でなければ空。
     */
    const std::string& composition() const { return composition_; }

    /**
     * @brief 変換対象の節の開始位置を返す。
     * @return 変換中文字列内のバイト位置。
     */
    size_t compositionTargetBegin() const { return compositionTargetBegin_; }

    /**
     * @brief 変換対象の節の終了位置を返す。
     * @return 変換中文字列内のバイト位置。開始位置以下なら対象の節が無い。
     */
    size_t compositionTargetEnd() const { return compositionTargetEnd_; }

    /**
     * @brief 変換中文字列を設定する。
     * @param[in] utf8        変換中文字列(UTF-8)。空なら変換なしとして扱う。
     * @param[in] caretBytes  変換中文字列内のキャレット位置(先頭からのバイト数)。
     * @param[in] targetBegin 変換対象の節の開始位置(同上)。
     * @param[in] targetEnd   変換対象の節の終了位置(同上)。
     * @note 位置はすべて変換中文字列の長さへクランプされる(IME が返す位置を
     *       そのまま信用しないため)。終了位置は開始位置を下回らない。
     */
    void setComposition(const std::string& utf8, size_t caretBytes, size_t targetBegin,
                        size_t targetEnd);

    /// @brief 変換中文字列を捨てる(変換のキャンセル・確定、編集の開始と終了)。
    void resetComposition();

    /**
     * @brief 描画に使うテキストを組み立てる。
     * @return 編集中の文字列に、変換中文字列をキャレット位置へ挿入したもの(UTF-8)。
     */
    std::string displayText() const;

    /**
     * @brief 描画に使う部分書式を組み立てる。
     * @return 編集中の部分書式を、変換中文字列の挿入分だけずらしたもの。
     * @note 変換中文字列そのものは直前の文字の書式を継ぐ(adjustTextStyles と同じ規則)。
     */
    std::vector<TextStyleRun> displayStyles() const;

    /**
     * @brief 表示用テキスト内でのキャレット位置を返す。
     * @return バイト位置。変換中は変換中文字列内のキャレットを加えた位置。
     */
    size_t caretOffset() const;

private:
    bool active_ = false;          ///< 編集中か
    size_t index_ = 0;             ///< 編集中の注釈 index
    bool created_ = false;         ///< 新規作成中(空のまま終了したら注釈を消す)
    TextEditBuffer buffer_;        ///< 編集中の文字列・キャレット・選択範囲・部分書式
    bool caretOn_ = true;          ///< キャレット点滅の表示相
    bool mouseSelecting_ = false;  ///< ドラッグで選択範囲を広げている最中
    bool styleMenuPending_ = false;      ///< 右ボタンを選択範囲の上で押した
    /// IME の変換中文字列(UTF-8)。確定するまで buffer_ には入れず、表示にだけ混ぜる
    std::string composition_;
    size_t compositionCaret_ = 0;        ///< 変換中文字列内のキャレット(バイト位置)
    size_t compositionTargetBegin_ = 0;  ///< 変換対象の節の開始(同上)
    size_t compositionTargetEnd_ = 0;    ///< 変換対象の節の終了(同上)
};

} // namespace blinker
