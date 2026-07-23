#pragma once

#include <string>
#include <vector>

#include "core/command.h"
#include "core/keymap.h"

/**
 * @file help.h
 * @brief 現在のキーバインドから操作一覧(ヘルプ)の表示テキストを組み立てる。
 *
 * 内容は固定のテキストではなく Keymap から生成する。blinker.ini の [keys] で
 * 割り当てを変えた場合も、実際に効くキーがそのまま出る。
 */

namespace blinker {

/// @brief 操作一覧の 1 行。
struct HelpLine {
    std::string text;     ///< 表示文字列(UTF-8)
    bool header = false;  ///< 見出し行なら true(呼び出し側で強調表示する)
};

/**
 * @brief 操作一覧の全行を組み立てる。
 * @param[in] keymap 表示対象のキーバインド。
 * @return 見出しと「操作名 キー」の行。キーが 1 つも割り当てられていない操作は含まれない。
 * @note キー表記は Keymap::chordToString と同じ(そのまま blinker.ini に書ける)。
 */
std::vector<HelpLine> buildHelpLines(const Keymap& keymap);

/**
 * @brief 指定コマンドに割り当てられたキーを 1 つの文字列にまとめる。
 * @param[in] keymap 検索対象のキーバインド。
 * @param[in] cmd    対象のコマンド。
 * @return 半角スペース区切りのキー表記("Right Down Space")。未割り当てなら空文字列。
 */
std::string keysLabel(const Keymap& keymap, Command cmd);

} // namespace blinker
