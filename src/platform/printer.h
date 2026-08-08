#pragma once

#include <string>

#include "platform/decoder.h"

/**
 * @file printer.h
 * @brief 印刷のプラットフォーム抽象。
 */

namespace blinker {

/**
 * @brief 印刷の設定。
 *
 * プリンタ・用紙・向き・部数はプラットフォームの印刷ダイアログで選ぶので持たない。
 * ここにあるのは「用紙のどこにどう置くか」だけで、値は blinker.ini の `[print]` に由来する。
 */
struct PrintOptions {
    /// 用紙の印刷可能領域からさらに空ける余白 (mm)。0 なら余白なし
    float marginMm = 10.0f;
    /// 画像と用紙の向きが違うとき、90 度回して大きく刷るか
    bool autoRotate = true;
};

/**
 * @brief 印刷の結果。
 *
 * 取りやめと失敗を分けてあるのは、取りやめでは通知を出さないため
 * (保存ダイアログのキャンセルと同じ扱い)。
 */
enum class PrintStatus {
    Printed,      ///< 印刷ジョブを送った
    Canceled,     ///< 利用者が印刷ダイアログで取りやめた
    Unsupported,  ///< このビルドでは印刷に対応していない
    Failed,       ///< プリンタへの出力に失敗した
};

/**
 * @brief 印刷のプラットフォーム抽象。
 *
 * Windows 実装は OS のモダン印刷ダイアログ + Direct2D (print_winrt) で、
 * それが使えない環境では GDI の印刷ダイアログ (printer_win) へ落ちる。
 * SDL バックエンドは未対応 (printer_stub)。
 * 画像は 1 ページに収まるよう拡大縮小して印刷する(タイル印刷・複数ページはしない)。
 */
class IPrinter {
public:
    virtual ~IPrinter() = default;

    /**
     * @brief 印刷ダイアログを表示し、選ばれたプリンタへ画像を 1 ページで印刷する。
     * @param[in] image   印刷する画像(32bpp PBGRA)。実装はアルファを見ないことがあるため、
     *                    半透明を含む画像は呼び出し側で背景へ焼き込んでおくこと。
     * @param[in] jobName 印刷キューに表示されるジョブ名(UTF-8)。空なら実装が既定名を使う。
     *                    ダイアログのタイトルにも使われることがある。
     * @param[in] options 余白・自動回転の設定。
     * @return 印刷結果。
     * @note ダイアログを含むモーダル処理で、応答があるまで返らない。UI スレッドから呼ぶこと。
     *       実装はその間もメッセージを回す(ダイアログのプレビュー描画を進めるため)ので、
     *       呼び出し中に他のコマンドが走らないよう窓を無効化するのは実装側の責任。
     */
    virtual PrintStatus print(const DecodedImage& image, const std::string& jobName,
                              const PrintOptions& options) = 0;
};

} // namespace blinker
