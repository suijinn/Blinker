#include "core/image_scale.h"

#include <algorithm>
#include <new>

namespace blinker {

std::shared_ptr<DecodedImage> downscaleToFit(const DecodedImage& image,
                                             const uint32_t maxDimension) {
    if (maxDimension == 0) return nullptr;
    const uint32_t srcW = image.width;
    const uint32_t srcH = image.height;
    if (srcW == 0 || srcH == 0) return nullptr;
    if (static_cast<size_t>(srcW) * srcH * 4 > image.pixels.size()) return nullptr;
    if (srcW <= maxDimension && srcH <= maxDimension) return nullptr;  // 縮小不要

    // 長い辺を maxDimension に合わせる(短い辺は比率のまま)
    const double scale = std::min(static_cast<double>(maxDimension) / srcW,
                                  static_cast<double>(maxDimension) / srcH);
    const uint32_t dstW =
        std::clamp(static_cast<uint32_t>(static_cast<double>(srcW) * scale), 1u, maxDimension);
    const uint32_t dstH =
        std::clamp(static_cast<uint32_t>(static_cast<double>(srcH) * scale), 1u, maxDimension);

    std::shared_ptr<DecodedImage> out;
    try {
        out = std::make_shared<DecodedImage>();
        out->pixels.resize(static_cast<size_t>(dstW) * dstH * 4);
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
    out->width = dstW;
    out->height = dstH;
    // 既に縮小済みの画像を更に縮めることもあるので、元の大きさは引き継ぐ
    out->sourceWidth = image.sourceWidth != 0 ? image.sourceWidth : srcW;
    out->sourceHeight = image.sourceHeight != 0 ? image.sourceHeight : srcH;

    const uint8_t* src = image.pixels.data();
    uint8_t* dst = out->pixels.data();
    for (uint32_t y = 0; y < dstH; ++y) {
        // 出力画素が覆う入力の行範囲 [sy0, sy1)。丸めで空にならないよう最低 1 行は取る
        const uint32_t sy0 = static_cast<uint32_t>(static_cast<uint64_t>(y) * srcH / dstH);
        const uint32_t sy1 =
            std::max(sy0 + 1, static_cast<uint32_t>(static_cast<uint64_t>(y + 1) * srcH / dstH));
        for (uint32_t x = 0; x < dstW; ++x) {
            const uint32_t sx0 = static_cast<uint32_t>(static_cast<uint64_t>(x) * srcW / dstW);
            const uint32_t sx1 = std::max(
                sx0 + 1, static_cast<uint32_t>(static_cast<uint64_t>(x + 1) * srcW / dstW));
            // 縮小率が大きいと画素数が数億に達するので合計は 64bit で持つ
            uint64_t sum[4] = {0, 0, 0, 0};
            for (uint32_t sy = sy0; sy < sy1; ++sy) {
                const uint8_t* p = src + (static_cast<size_t>(sy) * srcW + sx0) * 4;
                for (uint32_t sx = sx0; sx < sx1; ++sx, p += 4) {
                    sum[0] += p[0];
                    sum[1] += p[1];
                    sum[2] += p[2];
                    sum[3] += p[3];
                }
            }
            const uint64_t count = static_cast<uint64_t>(sy1 - sy0) * (sx1 - sx0);
            uint8_t* d = dst + (static_cast<size_t>(y) * dstW + x) * 4;
            for (int c = 0; c < 4; ++c) d[c] = static_cast<uint8_t>(sum[c] / count);
        }
    }
    return out;
}

} // namespace blinker
