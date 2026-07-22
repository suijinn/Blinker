#pragma once

#include <cstdint>

#include "platform/decoder.h"

/**
 * @file exif.h
 * @brief EXIF Orientation を画像へ適用する純 C++ 実装。
 */

namespace blinker {

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
