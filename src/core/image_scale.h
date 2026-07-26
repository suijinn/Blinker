#pragma once

#include <cstdint>
#include <memory>

#include "platform/decoder.h"

/**
 * @file image_scale.h
 * @brief デコード済み画像の縮小(純 C++ 実装)。
 */

namespace blinker {

/**
 * @brief 縦横が maxDimension 以下になるよう、縦横比を保って縮小したコピーを作る。
 *
 * 描画側の上限(D2D の `GetMaximumBitmapSize`、SDL の最大テクスチャサイズ)を
 * 超える画像を表示するために使う。縮小前の画像はそのまま残るので、保存・コピー・
 * 文字認識は元の大きさで行われる。
 *
 * 変換は出力画素ごとに対応する入力矩形を単純平均する箱型フィルタ。ピクセルは
 * アルファ事前乗算なのでチャンネルをそのまま平均してよい(非乗算だと透明部分の
 * 色が混ざる)。ガンマは無視している(sRGB 値のまま平均する)が、縮小の一般的な
 * 実装と同じで、ビューアの用途では差は見えない。
 *
 * @param[in] image        縮小元。32bpp PBGRA。
 * @param[in] maxDimension 許容する最大の辺の長さ(ピクセル)。
 * @return 縮小結果。既に収まっている場合・maxDimension が 0 の場合・入力が不正な
 *         場合・メモリ確保に失敗した場合は nullptr(呼び出し側は元の画像を使う)。
 */
std::shared_ptr<DecodedImage> downscaleToFit(const DecodedImage& image, uint32_t maxDimension);

} // namespace blinker
