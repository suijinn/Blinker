#include "core/exif.h"

#include <new>
#include <utility>

namespace blinker {

bool applyExifOrientation(DecodedImage& image, uint16_t orientation) {
    if (orientation <= 1 || orientation > 8) return false;
    const size_t w = image.width;
    const size_t h = image.height;
    if (w == 0 || h == 0 || image.pixels.size() < w * h * 4) return false;

    // 5〜8 は転置を伴うため出力の縦横が入れ替わる
    const bool transposed = orientation >= 5;
    const size_t dstW = transposed ? h : w;
    const size_t dstH = transposed ? w : h;

    std::vector<uint8_t> out;
    try {
        out.resize(w * h * 4);
    } catch (const std::bad_alloc&) {
        return false;
    }

    const uint8_t* src = image.pixels.data();
    uint8_t* dst = out.data();
    for (size_t y = 0; y < dstH; ++y) {
        for (size_t x = 0; x < dstW; ++x) {
            // 出力画素 (x, y) の取得元 (sx, sy) を求める
            size_t sx = 0;
            size_t sy = 0;
            switch (orientation) {
            case 2: sx = w - 1 - x; sy = y;             break;  // 左右反転
            case 3: sx = w - 1 - x; sy = h - 1 - y;     break;  // 180 度
            case 4: sx = x;         sy = h - 1 - y;     break;  // 上下反転
            case 5: sx = y;         sy = x;             break;  // 主対角で転置
            case 6: sx = y;         sy = h - 1 - x;     break;  // 時計回り 90 度
            case 7: sx = w - 1 - y; sy = h - 1 - x;     break;  // 副対角で転置
            case 8: sx = w - 1 - y; sy = x;             break;  // 時計回り 270 度
            default: break;
            }
            const uint8_t* s = src + (sy * w + sx) * 4;
            uint8_t* d = dst + (y * dstW + x) * 4;
            d[0] = s[0];
            d[1] = s[1];
            d[2] = s[2];
            d[3] = s[3];
        }
    }

    image.pixels = std::move(out);
    image.width = static_cast<uint32_t>(dstW);
    image.height = static_cast<uint32_t>(dstH);
    return true;
}

} // namespace blinker
