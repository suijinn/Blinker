#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/text_style.h"

/**
 * @file text_edit.h
 * @brief 画像上で直接テキストを編集するための編集バッファ(キャレット・選択範囲・部分書式)。
 *
 * OS ヘッダに依存しない(単体テスト対象)。文字列は UTF-8、位置はすべて
 * UTF-8 のバイト位置で表す。行の折り返しやフォント計測には関与しないため、
 * 上下移動やクリックでのキャレット移動は呼び出し側が
 * IAnnotationRasterizer のテキスト計測でバイト位置を求めて setCaret する。
 */

namespace blinker {

/**
 * @brief UTF-8 文字列に対するキャレット付き編集バッファ。
 *
 * キャレット位置 caret() と選択の始点 anchor() を持ち、両者が等しいときは
 * 選択なし。位置は常にコードポイント境界に丸められる。
 * 部分書式(色・太字・斜体・下線)も保持し、文字列を変更するたびに位置を追従させる。
 */
class TextEditBuffer {
public:
    /// @brief 空の文字列で構築する。
    TextEditBuffer() = default;

    /**
     * @brief 文字列と部分書式を受け取り、キャレットを末尾に置いて構築する。
     * @param[in] text   初期文字列(UTF-8。改行 LF)。
     * @param[in] styles 初期の部分書式。位置は text 内の UTF-8 バイト位置。
     */
    explicit TextEditBuffer(std::string text, std::vector<TextStyleRun> styles = {});

    /**
     * @brief 編集中の文字列を返す。
     * @return 現在の文字列(UTF-8)。
     */
    const std::string& text() const { return text_; }

    /**
     * @brief 部分書式(色・太字・斜体・下線)の範囲リストを返す。
     * @return 範囲リスト。位置は text() 内の UTF-8 バイト位置。
     */
    const std::vector<TextStyleRun>& styles() const { return styles_; }

    /**
     * @brief キャレット位置を返す。
     * @return キャレットのバイト位置。
     */
    size_t caret() const { return caret_; }

    /**
     * @brief 選択の始点(キャレットと反対側の端)を返す。
     * @return アンカーのバイト位置。選択がなければ caret() と等しい。
     */
    size_t anchor() const { return anchor_; }

    /**
     * @brief 選択範囲があるかを返す。
     * @return 1 文字以上選択されていれば true。
     */
    bool hasSelection() const { return caret_ != anchor_; }

    /**
     * @brief 選択範囲の開始位置を返す。
     * @return caret() と anchor() の小さい方(バイト位置)。
     */
    size_t selectionBegin() const { return caret_ < anchor_ ? caret_ : anchor_; }

    /**
     * @brief 選択範囲の終了位置を返す。
     * @return caret() と anchor() の大きい方(バイト位置)。
     */
    size_t selectionEnd() const { return caret_ < anchor_ ? anchor_ : caret_; }

    /**
     * @brief 選択されている文字列を返す。
     * @return 選択範囲の文字列(UTF-8)。選択がなければ空文字列。
     */
    std::string selectedText() const;

    /**
     * @brief キャレットを指定位置へ移動する。
     * @param[in] offset          移動先のバイト位置。範囲外は端へ、コードポイントの
     *                            途中はその先頭へ丸める。
     * @param[in] extendSelection true ならアンカーを保って選択を広げる。false なら選択解除。
     */
    void setCaret(size_t offset, bool extendSelection);

    /**
     * @brief 文字列を挿入する。選択範囲があれば置き換える。
     * @param[in] utf8 挿入する文字列(UTF-8)。
     * @note 挿入後のキャレットは挿入した文字列の直後に移動し、選択は解除される。
     */
    void insert(std::string_view utf8);

    /**
     * @brief 選択範囲を削除する。
     * @return 削除した場合は true。選択がなければ false(何もしない)。
     */
    bool deleteSelection();

    /**
     * @brief Backspace 相当の削除を行う。
     * @return 文字列が変化したら true。先頭で選択もなければ false。
     * @note 選択範囲があるときはそれを削除する。
     */
    bool backspace();

    /**
     * @brief Delete 相当の削除を行う。
     * @return 文字列が変化したら true。末尾で選択もなければ false。
     * @note 選択範囲があるときはそれを削除する。
     */
    bool deleteForward();

    /**
     * @brief キャレットを 1 コードポイント左へ移動する。
     * @param[in] extendSelection true なら選択を広げる。false かつ選択中なら選択の左端へ寄せる。
     */
    void moveLeft(bool extendSelection);

    /**
     * @brief キャレットを 1 コードポイント右へ移動する。
     * @param[in] extendSelection true なら選択を広げる。false かつ選択中なら選択の右端へ寄せる。
     */
    void moveRight(bool extendSelection);

    /**
     * @brief キャレットを現在行の先頭へ移動する。
     * @param[in] extendSelection true なら選択を広げる。
     */
    void moveLineStart(bool extendSelection);

    /**
     * @brief キャレットを現在行の末尾へ移動する。
     * @param[in] extendSelection true なら選択を広げる。
     */
    void moveLineEnd(bool extendSelection);

    /// @brief 全体を選択する(キャレットは末尾、アンカーは先頭)。
    void selectAll();

    /**
     * @brief 選択範囲全体に属性が適用されているかを返す。
     * @param[in] flag 調べる属性。
     * @return 選択範囲の全体に適用されていれば true。選択がなければ false。
     */
    bool selectionHasFlag(TextStyleFlag flag) const;

    /**
     * @brief 選択範囲の属性を反転する(太字・斜体・下線のトグル)。
     * @param[in] flag 反転する属性。
     * @return 書式が変化したら true。選択がなければ false(何もしない)。
     * @note 選択範囲の全体に適用済みなら解除、そうでなければ全体へ適用する
     *       (一部だけ太字の範囲は、まず全体が太字になる)。
     */
    bool toggleSelectionFlag(TextStyleFlag flag);

    /**
     * @brief 選択範囲に文字色を設定する。
     * @param[in] colorRGB 設定する色(0xRRGGBB)。
     * @return 書式が変化したら true。選択がなければ false(何もしない)。
     */
    bool setSelectionColor(uint32_t colorRGB);

    /**
     * @brief 選択範囲にフォントを設定する。
     * @param[in] family フォントファミリ名(UTF-8)。空文字列なら指定を外し、
     *                   注釈全体のフォントで描かれるようになる(書式メニューで
     *                   注釈全体と同じフォントを選んだときはこちらを使う)。
     * @return 書式が変化したら true。選択がなければ false(何もしない)。
     */
    bool setSelectionFontFamily(const std::string& family);

    /**
     * @brief 選択範囲の先頭に適用されている書式を返す。
     * @return 選択の開始位置を含む範囲。書式が無ければ既定の書式。
     * @note メニューに現在の文字色・フォントを表示するために使う。
     */
    TextStyleRun selectionStyle() const;

    /**
     * @brief 指定位置にある「語」を選択する(ダブルクリック用)。
     * @param[in] offset 語を探す基準のバイト位置。
     * @note 空白の連なり・ASCII の英数字とアンダースコアの連なり・それ以外(記号や
     *       日本語など)の連なりを、それぞれ 1 語として扱う。
     */
    void selectWordAt(size_t offset);

private:
    /**
     * @brief 位置をコードポイント境界へ切り下げ、範囲内へ収める。
     * @param[in] offset 丸める前のバイト位置。
     * @return 丸められたバイト位置。
     */
    size_t clampToBoundary(size_t offset) const;

    std::string text_;                  ///< 編集中の文字列(UTF-8)
    std::vector<TextStyleRun> styles_;  ///< 部分書式。文字列の変更に追従させる
    size_t caret_ = 0;                  ///< キャレットのバイト位置
    size_t anchor_ = 0;                 ///< 選択の始点のバイト位置
};

} // namespace blinker
