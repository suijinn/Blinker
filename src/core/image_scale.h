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

/// @brief resizeImage が受け付ける 1 辺の最大値(デコーダの取り込み上限と同じ)。
inline constexpr uint32_t kMaxResizeDimension = 16384;

/**
 * @brief 任意の大きさへ拡大縮小したコピーを作る(利用者が指示するリサイズ)。
 *
 * 水平・垂直の 2 パスに分けた三角(テント)フィルタでリサンプルする。フィルタ半径を
 * 各軸で `max(1, 1/倍率)` にすることで、縮小では面積平均相当・拡大ではバイリニアと
 * 同じ結果になり、拡大と縮小を 1 本のコードで扱える(等倍を指定すると入力と同じ
 * ピクセルが返る)。ピクセルはアルファ事前乗算なのでチャンネルをそのまま加重平均して
 * よい(非乗算だと透明部分の色が混ざる)。
 *
 * 表示のために縮める downscaleToFit とは用途が別で、あちらは巨大画像を開くたびに
 * 通る性能の効く経路なので箱型フィルタのまま残してある。
 *
 * @param[in] src    変換元。32bpp PBGRA。
 * @param[in] width  変換後の幅(ピクセル)。
 * @param[in] height 変換後の高さ(ピクセル)。
 * @return 変換結果。幅か高さが 0、1 辺が kMaxResizeDimension 超、入力が不正、
 *         メモリ確保に失敗のいずれかで nullptr(上限いっぱいの 16384 角は PBGRA で
 *         1GB になるため、実際には確保の失敗で断ることがある)。
 * @note 入力が DecodedImage::downscaled() なら sourceWidth/sourceHeight を引き継ぐ
 *       (元ファイルの画素を持っていないことは変わらないので、上書き保存の拒否を
 *       維持する必要がある)。そうでなければ 0 のまま = 等倍扱いで、リサイズ後の
 *       上書き保存は通常どおり確認だけで通る。
 */
std::shared_ptr<DecodedImage> resizeImage(const DecodedImage& src, uint32_t width,
                                          uint32_t height);

} // namespace blinker
