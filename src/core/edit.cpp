#include "core/edit.h"

#include <algorithm>
#include <cstring>

namespace blinker {

std::shared_ptr<DecodedImage> cropImage(const DecodedImage& src, RectI rect) {
    const int x0 = std::max(rect.x, 0);
    const int y0 = std::max(rect.y, 0);
    const int x1 = std::min(rect.x + rect.w, static_cast<int>(src.width));
    const int y1 = std::min(rect.y + rect.h, static_cast<int>(src.height));
    if (x1 <= x0 || y1 <= y0) return nullptr;

    auto result = std::make_shared<DecodedImage>();
    result->width = static_cast<uint32_t>(x1 - x0);
    result->height = static_cast<uint32_t>(y1 - y0);
    result->pixels.resize(static_cast<size_t>(result->width) * result->height * 4);
    const size_t srcStride = static_cast<size_t>(src.width) * 4;
    const size_t dstStride = static_cast<size_t>(result->width) * 4;
    for (int y = y0; y < y1; ++y) {
        std::memcpy(result->pixels.data() + static_cast<size_t>(y - y0) * dstStride,
                    src.pixels.data() + static_cast<size_t>(y) * srcStride +
                        static_cast<size_t>(x0) * 4,
                    dstStride);
    }
    return result;
}

void blendOverlay(DecodedImage& dst, const DecodedImage& overlay, int x, int y) {
    const int x0 = std::max(x, 0);
    const int y0 = std::max(y, 0);
    const int x1 = std::min(x + static_cast<int>(overlay.width), static_cast<int>(dst.width));
    const int y1 = std::min(y + static_cast<int>(overlay.height), static_cast<int>(dst.height));
    for (int dy = y0; dy < y1; ++dy) {
        const uint8_t* srcRow = overlay.pixels.data() +
                                (static_cast<size_t>(dy - y) * overlay.width + (x0 - x)) * 4;
        uint8_t* dstRow = dst.pixels.data() + (static_cast<size_t>(dy) * dst.width + x0) * 4;
        for (int dx = x0; dx < x1; ++dx, srcRow += 4, dstRow += 4) {
            const uint8_t a = srcRow[3];
            if (a == 0) continue;
            if (a == 255) {
                dstRow[0] = srcRow[0];
                dstRow[1] = srcRow[1];
                dstRow[2] = srcRow[2];
                dstRow[3] = 255;
                continue;
            }
            // 事前乗算どうしの over: out = src + dst * (1 - srcA)
            const unsigned inv = 255u - a;
            for (int c = 0; c < 4; ++c) {
                dstRow[c] = static_cast<uint8_t>(srcRow[c] + (dstRow[c] * inv + 127u) / 255u);
            }
        }
    }
}

} // namespace blinker
