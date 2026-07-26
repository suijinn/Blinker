#pragma once

#include <string>
#include <vector>

#include "core/command.h"
#include "core/keymap.h"
#include "core/mousemap.h"

/**
 * @file help.h
 * @brief 現在のキー・マウスの割り当てから操作一覧(ヘルプ)の表示テキストを組み立てる。
 *
 * 内容は固定のテキストではなく Keymap / Mousemap から生成する。blinker.ini の
 * [keys] や [mouse] で割り当てを変えた場合も、実際に効く操作がそのまま出る。
 */

namespace blinker {

/// @brief 操作一覧の 1 行。
struct HelpLine {
    std::string text;     ///< 表示文字列(UTF-8)
    bool header = false;  ///< 見出し行なら true(呼び出し側で強調表示する)
};

/**
 * @brief 操作一覧の全行を組み立てる。
 * @param[in] keymap            表示対象のキーバインド。
 * @param[in] mousemap          表示対象のマウス割り当て(「マウス」の節に出す)。
 * @param[in] swapMouseButtons  マウスの左右の役割を入れ替えているか
 *                              (`[mouse] swap_buttons`)。マウス操作の行の
 *                              「左ドラッグ」「右ドラッグ」の表記に反映する。
 * @return 見出しと「操作名 キー」の行。キーが 1 つも割り当てられていない操作は含まれない。
 * @note キー表記は Keymap::chordToString と同じ(そのまま blinker.ini に書ける)。
 *       マウスの表記だけは日本語(Mousemap::chordToDisplayString)で、ini には書けない。
 */
std::vector<HelpLine> buildHelpLines(const Keymap& keymap, const Mousemap& mousemap,
                                     bool swapMouseButtons);

/**
 * @brief 指定コマンドに割り当てられたキーを 1 つの文字列にまとめる。
 * @param[in] keymap 検索対象のキーバインド。
 * @param[in] cmd    対象のコマンド。
 * @return 半角スペース区切りのキー表記("Right Down Space")。未割り当てなら空文字列。
 */
std::string keysLabel(const Keymap& keymap, Command cmd);

/**
 * @brief 指定コマンドに割り当てられたマウス操作を 1 つの文字列にまとめる。
 * @param[in] mousemap 検索対象のマウス割り当て。
 * @param[in] cmd      対象のコマンド。
 * @return 半角スペース区切りの日本語表記("サイドボタン(進む) Ctrl+ホイール↓")。
 *         未割り当てなら空文字列。
 */
std::string mouseLabel(const Mousemap& mousemap, Command cmd);

} // namespace blinker
