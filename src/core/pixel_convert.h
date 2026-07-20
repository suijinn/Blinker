#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "platform/decoder.h"

/**
 * @file pixel_convert.h
 * @brief PBGRA(事前乗算)から保存用ピクセル形式への変換。
 */

namespace blinker {

/**
 * @brief 事前乗算値からストレート値へ戻す(四捨五入)。
 * @param[in] value 事前乗算済みのチャンネル値。
 * @param[in] alpha 同じピクセルのアルファ値。
 * @return ストレートアルファでのチャンネル値。alpha が 0 なら 0。
 */
inline uint8_t unpremultiply(uint8_t value, uint8_t alpha) {
    if (alpha == 0) return 0;
    return static_cast<uint8_t>(std::min(255u, (value * 255u + alpha / 2u) / alpha));
}

/**
 * @brief PBGRA をストレートアルファの 32bpp BGRA へ変換する(PNG 保存用)。
 * @param[in] image 変換元の画像(32bpp PBGRA)。
 * @return 変換されたピクセルバッファ。stride = width * 4。
 */
std::vector<uint8_t> toStraightBGRA(const DecodedImage& image);

/**
 * @brief PBGRA を白背景に合成した 24bpp BGR へ変換する(JPEG/BMP 保存用)。
 * @param[in] image 変換元の画像(32bpp PBGRA)。
 * @return 変換されたピクセルバッファ。stride = width * 3(パディングなし)。
 */
std::vector<uint8_t> toOpaqueBGR(const DecodedImage& image);

} // namespace blinker
