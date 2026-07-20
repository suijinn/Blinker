#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

/**
 * @file text_style.h
 * @brief テキスト内の部分書式(色・太字・斜体・下線)を表す範囲リストと、その編集の純関数。
 *
 * OS ヘッダに依存しない(単体テスト対象)。位置はすべて UTF-8 のバイト位置で、
 * 対象の文字列とは独立に保持する(文字列側は TextEditBuffer / AnnotationSpec::text)。
 *
 * 範囲リストは常に「開始位置の昇順・重なりなし・既定のままの範囲は持たない」
 * 正規形で保つ。書式を指定していない部分は AnnotationSpec の colorRGB と
 * 標準の字形(太字・斜体・下線なし)で描かれる。
 */

namespace blinker {

/**
 * @brief 部分書式を適用する 1 範囲。
 *
 * [begin, end) の半開区間。hasColor が false の範囲は色を上書きせず、
 * 注釈全体の色(AnnotationSpec::colorRGB)のまま描かれる。
 */
struct TextStyleRun {
    size_t begin = 0;       ///< 範囲の開始バイト位置(この位置を含む)
    size_t end = 0;         ///< 範囲の終了バイト位置(この位置を含まない)
    bool bold = false;      ///< 太字にするか
    bool italic = false;    ///< 斜体にするか
    bool underline = false; ///< 下線を引くか
    bool hasColor = false;  ///< colorRGB を使うか。false なら注釈全体の色
    uint32_t colorRGB = 0;  ///< 文字色(0xRRGGBB)。hasColor が false なら無意味

    /**
     * @brief 全メンバの一致で比較する。
     * @param[in] other 比較相手。
     * @return 全メンバが等しければ true。
     */
    bool operator==(const TextStyleRun& other) const = default;
};

/// @brief 真偽で表す書式属性(色以外)。
enum class TextStyleFlag {
    Bold,      ///< 太字
    Italic,    ///< 斜体
    Underline, ///< 下線
};

/**
 * @brief 範囲リストを正規形へ整える。
 *
 * 空の範囲と既定のままの範囲(太字・斜体・下線・色のいずれも指定なし)を捨て、
 * 開始位置の昇順に並べ、隣接する同じ書式の範囲を 1 つにまとめる。
 *
 * @param[in,out] runs 整える範囲リスト。破壊的に更新される。
 * @note 他の関数は入力が正規形であることを前提にせず、内部で必要に応じて呼ぶ。
 */
void normalizeTextStyles(std::vector<TextStyleRun>& runs);

/**
 * @brief 指定位置に適用されている書式を返す。
 * @param[in] runs   範囲リスト。
 * @param[in] offset 調べるバイト位置。
 * @return offset を含む範囲。どの範囲にも含まれなければ既定の書式
 *         (begin = end = offset、太字・斜体・下線・色すべて指定なし)。
 */
TextStyleRun textStyleAt(const std::vector<TextStyleRun>& runs, size_t offset);

/**
 * @brief 範囲全体に属性が適用されているかを返す。
 * @param[in] runs  範囲リスト。
 * @param[in] begin 調べる範囲の開始バイト位置。
 * @param[in] end   調べる範囲の終了バイト位置。
 * @param[in] flag  調べる属性。
 * @return [begin, end) の全バイトに flag が適用されていれば true。
 *         範囲が空(end <= begin)なら false。
 * @note 「選択範囲が既に太字なら解除する」というトグルの判定に使う。
 */
bool isTextStyleFlagSet(const std::vector<TextStyleRun>& runs, size_t begin, size_t end,
                        TextStyleFlag flag);

/**
 * @brief 範囲へ属性を設定・解除する。
 * @param[in,out] runs    更新する範囲リスト。正規形で返る。
 * @param[in]     begin   対象範囲の開始バイト位置。
 * @param[in]     end     対象範囲の終了バイト位置。end <= begin なら何もしない。
 * @param[in]     flag    設定する属性。
 * @param[in]     enabled true で適用、false で解除。
 * @note 範囲外の書式や、同じ範囲の別の属性は保たれる(色・太字・斜体・下線は
 *       それぞれ独立に指定できる)。
 */
void setTextStyleFlag(std::vector<TextStyleRun>& runs, size_t begin, size_t end,
                      TextStyleFlag flag, bool enabled);

/**
 * @brief 範囲へ文字色を設定する。
 * @param[in,out] runs     更新する範囲リスト。正規形で返る。
 * @param[in]     begin    対象範囲の開始バイト位置。
 * @param[in]     end      対象範囲の終了バイト位置。end <= begin なら何もしない。
 * @param[in]     colorRGB 設定する色(0xRRGGBB)。
 * @note 同じ範囲の太字・斜体・下線は保たれる。
 */
void setTextStyleColor(std::vector<TextStyleRun>& runs, size_t begin, size_t end,
                       uint32_t colorRGB);

/**
 * @brief 文字列の編集に合わせて範囲の位置をずらす。
 *
 * offset から removed バイトを削除し、同じ位置へ inserted バイトを挿入した、
 * という 1 回の編集に追従させる。
 *
 * @param[in,out] runs     更新する範囲リスト。正規形で返る。
 * @param[in]     offset   編集した位置(バイト)。
 * @param[in]     removed  削除したバイト数。
 * @param[in]     inserted 挿入したバイト数。
 * @note 挿入位置に接する範囲のうち、直前で終わっている範囲が挿入分を取り込む
 *       (一般的なエディタと同じく、入力した文字は直前の文字の書式を継ぐ)。
 */
void adjustTextStyles(std::vector<TextStyleRun>& runs, size_t offset, size_t removed,
                      size_t inserted);

} // namespace blinker
