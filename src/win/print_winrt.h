#pragma once

#include <windows.h>

#include <string>

#include "platform/decoder.h"
#include "platform/printer.h"

/**
 * @file print_winrt.h
 * @brief OS のモダン印刷ダイアログ(プレビュー付き)による印刷。
 */

namespace blinker {

/// @brief モダン印刷ダイアログでの印刷結果。
enum class ModernPrintStatus {
    Printed,      ///< 印刷ジョブを送った
    Canceled,     ///< 利用者がダイアログで取りやめた
    Failed,       ///< ダイアログは出たが出力に失敗した
    Unavailable,  ///< この環境では使えない(呼び出し側は従来の印刷ダイアログへ落とすこと)
};

/**
 * @brief OS のモダン印刷ダイアログを出し、選ばれたプリンタへ画像を 1 ページで印刷する。
 *
 * Windows 8 以降の印刷 UI (`Windows.Graphics.Printing.PrintManager`) を
 * `IPrintManagerInterop` 経由でデスクトップアプリから開く。**プレビュー枠の中身は
 * OS ではなくこちらが描く**(`IPrintDocumentPageSource` / `IPrintPreviewPageCollection` を
 * 実装し、OS から渡される DXGI サーフェスへ Direct2D で用紙 1 枚を描く)。
 * 実装していないアプリの印刷ダイアログには「このアプリは印刷プレビューを
 * サポートしていません」と出る ―― それを埋めるための経路。
 *
 * @param[in] image   印刷する画像(32bpp PBGRA)。用紙は白なので、半透明を含む画像は
 *                    呼び出し側で背景へ焼き込んでおくこと。
 * @param[in] owner   親ウィンドウ。印刷 UI の持ち主になる。nullptr は不可
 *                    (ウィンドウを要求する API のため ModernPrintStatus::Unavailable を返す)。
 * @param[in] jobName 印刷キューとダイアログに表示されるジョブ名。
 * @param[in] options 余白・自動回転の設定。用紙・向き・部数はダイアログが持つ。
 * @return 印刷結果。ダイアログを出せなかった場合は ModernPrintStatus::Unavailable。
 * @note モーダル。UI スレッド(STA)から呼ぶこと。表示中はメッセージを回し続ける
 *       (止めるとダイアログもプレビューの描画要求も進まない)。
 */
ModernPrintStatus printWithModernUi(HWND owner, const DecodedImage& image,
                                    const std::wstring& jobName, const PrintOptions& options);

} // namespace blinker
