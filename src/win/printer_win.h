#pragma once

#include <windows.h>

#include <string>

#include "platform/printer.h"

/**
 * @file printer_win.h
 * @brief 印刷(Windows / GDI 実装)。
 */

namespace blinker {

/**
 * @brief Windows の印刷。
 *
 * まず OS のモダン印刷ダイアログ(プレビュー付き。print_winrt)を試し、それが
 * 使えない環境でだけ従来の PrintDlg + GDI へ落ちる。GDI 経路では選ばせたプリンタの
 * DC へ画像を StretchDIBits で 1 ページ描く(プレビューは出ない)。
 * どちらの経路でもプリンタ・用紙・向き・部数はダイアログの設定に従い、
 * 用紙のどこに置くかだけを PrintOptions(blinker.ini の `[print]`)で決める。
 */
class PrinterWin final : public IPrinter {
public:
    /**
     * @brief 印刷ダイアログの親ウィンドウを設定する。
     * @param[in] owner 親ウィンドウ。nullptr でも動くがモーダルにならない。
     */
    void setOwner(HWND owner) { owner_ = owner; }

    /**
     * @brief 印刷ダイアログを表示し、選ばれたプリンタへ画像を 1 ページで印刷する。
     * @param[in] image   印刷する画像(32bpp PBGRA)。GDI はアルファを見ないため、
     *                    半透明を含む画像は呼び出し側で背景へ焼き込んでおくこと。
     * @param[in] jobName 印刷キューに表示されるジョブ名(UTF-8)。空なら "Blinker"。
     *                    モダン印刷ダイアログではタイトルにもなる。
     * @param[in] options 余白・自動回転の設定。
     * @return 印刷結果。ダイアログでの取りやめは PrintStatus::Canceled。
     * @note モーダル。UI スレッドから呼ぶこと。モダン経路では表示中もメッセージを回し、
     *       その間は親ウィンドウを無効にする。
     */
    PrintStatus print(const DecodedImage& image, const std::string& jobName,
                      const PrintOptions& options) override;

private:
    HWND owner_ = nullptr;  ///< 印刷ダイアログの親ウィンドウ
};

} // namespace blinker
