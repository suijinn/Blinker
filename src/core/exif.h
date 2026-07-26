#pragma once

#include <cstddef>
#include <cstdint>

#include "platform/decoder.h"

/**
 * @file exif.h
 * @brief EXIF Orientation の読み取りと画像への適用(純 C++ 実装)。
 */

namespace blinker {

/**
 * @brief 画像ファイルのバイト列から EXIF Orientation を読む。
 *
 * JPEG の APP1 (Exif) セグメントと PNG の eXIf チャンクに対応する。
 * WIC のようにメタデータを読めるデコーダを持たない SDL バックエンド
 * (`DecoderStb`)のために自前で解析する。壊れたデータに対しては失敗させず
 * 「回転なし」を返す(表示できる画像を回転のためだけに捨てないため)。
 *
 * @param[in] data ファイル先頭からのバイト列。nullptr なら 1 を返す。
 * @param[in] size data の長さ(バイト)。ファイル全体でなくてもよいが、
 *                 Exif セグメントが途中で切れていれば 1 を返す。
 * @return Orientation 値 (1〜8)。見つからない・解析できない場合は 1(回転なし)。
 */
uint16_t readExifOrientation(const uint8_t* data, size_t size);

/**
 * @brief EXIF Orientation に従って画像を回転・反転する(32bpp ピクセル用)。
 *
 * 回転を自前で行うのは、WIC の `IWICBitmapFlipRotator` をコーデックへ直結すると
 * 90/270 度回転で出力行ごとにソースを引き直し、大きな JPEG では事実上停止するため。
 * デコード後の連続バッファ上で回せば画素数に比例した時間で済む。
 *
 * orientation が 5〜8 のときは縦横が入れ替わり、image の width/height も更新される。
 * 1 や範囲外の値では何もしない。
 *
 * @param[in,out] image       回転対象。32bpp(stride = width * 4)であること。
 * @param[in]     orientation EXIF Orientation 値 (1〜8)。
 * @return 回転・反転を行ったら true。何もしなかったら false。
 * @note ピクセル形式には依存しない(4 バイト単位でそのまま移すため PBGRA/RGBA どちらでも可)。
 */
bool applyExifOrientation(DecodedImage& image, uint16_t orientation);

} // namespace blinker
