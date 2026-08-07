#pragma once

#include <filesystem>
#include <vector>

/**
 * @file drag_drop_win.h
 * @brief ファイルのドラッグ&ドロップ元(OLE の DoDragDrop)。
 */

namespace blinker {

/**
 * @brief ファイルを CF_HDROP でドラッグし、落とされるまで待つ。
 *
 * サイドバーの項目を掴んでエクスプローラや他のアプリへ渡すために使う。
 * 落とし先へ許すのはコピーとリンクだけで、移動は許さない(閲覧しているファイルが
 * ドラッグひとつで消えるのを防ぐ)。
 *
 * @param[in] paths 渡すファイルのパス。空なら何もしない。相対パスは絶対パスにして渡す。
 * @note **呼び出しの間ずっと戻らない**(OLE がマウスを握り、ドロップかキャンセルで返る)。
 *       UI スレッド(STA)から呼ぶこと。呼ぶ前に OleInitialize が済んでいる必要がある。
 *       DoDragDrop 自身がマウスを捕捉するので、呼び出し側は先にキャプチャを手放すこと。
 */
void dragFiles(const std::vector<std::filesystem::path>& paths);

}  // namespace blinker
