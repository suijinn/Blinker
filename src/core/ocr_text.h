#pragma once

#include <string>
#include <string_view>

#include "platform/ocr.h"

/**
 * @file ocr_text.h
 * @brief OCR の認識結果の後処理(テキスト整形と、読み直しの判断)。
 *
 * OS ヘッダに依存しない(単体テスト対象)。
 */

namespace blinker {

/**
 * @brief CJK 文字に挟まれた半角空白を取り除く。
 *
 * Windows の OCR は日本語でも単語の区切りに空白を入れて返すため、そのままでは
 * 「これ は 文字 認識 です」のようになる。両隣がどちらも CJK 文字である空白だけを
 * 落とすので、「ABC あいう」のような和欧混在の区切りは残る。
 *
 * @param[in] s 対象の文字列(UTF-8)。
 * @return 空白を取り除いた文字列(UTF-8)。
 * @note 連続する空白は 1 つでも複数でも、両端が CJK なら全体を取り除く。
 */
std::string removeSpacesBetweenCjk(std::string_view s);

/**
 * @brief 認識結果を 1 つのテキストへ整形する。
 *
 * 各行の前後の空白を落とし、空行を除いて改行 (LF) で連結したうえで
 * removeSpacesBetweenCjk を適用する。
 *
 * @param[in] result 整形する認識結果。
 * @return 整形されたテキスト(UTF-8)。認識行が無ければ空文字列。
 */
std::string ocrResultToText(const OcrResult& result);

/**
 * @brief 拡大して認識をやり直すべきかを判断し、その拡大率を返す。
 *
 * 文字が小さいと認識精度が大きく落ちる。どれだけ小さいかは 1 回認識してみないと
 * 分からないため、1 回目に得た行の高さから 2 回目の拡大率を決める。
 *
 * 実測(15 枚のテストセット、9〜13pt・5 書体)では、この読み直しで平均精度が
 * 71.5% → 84.2% に上がり、所要時間は約 2 倍(1 枚あたり 100ms 未満)になった。
 * 目標行高は 22〜34px のどこでも 80〜85% で、値による差はリサンプルのジッタで
 * 優劣が付かなかったため範囲の中央の 30px を採っている。拡大しすぎると
 * かえって落ちる(48px を狙うと 73.1% と、読み直さない場合とほぼ変わらなくなる)。
 *
 * @param[in] lineHeightPx 1 回目に認識した行の高さの中央値(認識に渡した
 *                         ビットマップ上のピクセル)。0 以下なら行が無かったことを表す。
 * @param[in] maxFactor    拡大率の上限。認識器が受け付ける画像サイズから
 *                         呼び出し側が算出する。
 * @return 2 回目に使う拡大率。1.0 なら読み直す価値がない(1 回目の結果を使う)。
 */
double ocrRetryUpscale(int lineHeightPx, double maxFactor);

} // namespace blinker
