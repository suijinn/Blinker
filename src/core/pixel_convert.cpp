#include "core/pixel_convert.h"

namespace blinker {

std::vector<uint8_t> toStraightBGRA(const DecodedImage& image) {
    std::vector<uint8_t> out(image.pixels.size());
    for (size_t i = 0; i < image.pixels.size(); i += 4) {
        const uint8_t a = image.pixels[i + 3];
        out[i + 0] = unpremultiply(image.pixels[i + 0], a);
        out[i + 1] = unpremultiply(image.pixels[i + 1], a);
        out[i + 2] = unpremultiply(image.pixels[i + 2], a);
        out[i + 3] = a;
    }
    return out;
}

std::vector<uint8_t> toOpaqueBGR(const DecodedImage& image) {
    std::vector<uint8_t> out(static_cast<size_t>(image.width) * image.height * 3);
    for (size_t px = 0; px < static_cast<size_t>(image.width) * image.height; ++px) {
        const uint8_t* src = image.pixels.data() + px * 4;
        const uint8_t a = src[3];
        // 事前乗算値に白の寄与 (255 - a) を足すだけで白背景合成になる
        for (int c = 0; c < 3; ++c) {
            out[px * 3 + c] =
                static_cast<uint8_t>(std::min(255u, static_cast<uint32_t>(src[c]) + (255u - a)));
        }
    }
    return out;
}

} // namespace blinker
