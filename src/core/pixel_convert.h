#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "platform/decoder.h"

namespace blinker {

// 事前乗算値からストレート値へ戻す(四捨五入)
inline uint8_t unpremultiply(uint8_t value, uint8_t alpha) {
    if (alpha == 0) return 0;
    return static_cast<uint8_t>(std::min(255u, (value * 255u + alpha / 2u) / alpha));
}

// PBGRA → ストレートアルファの 32bpp BGRA(PNG 保存用)。stride = width * 4
std::vector<uint8_t> toStraightBGRA(const DecodedImage& image);

// PBGRA → 白背景に合成した 24bpp BGR(JPEG/BMP 保存用)。stride = width * 3(詰め)
std::vector<uint8_t> toOpaqueBGR(const DecodedImage& image);

} // namespace blinker
