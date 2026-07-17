#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace blinker {

// デコード済み画像。ピクセルは 32bit BGRA(アルファ事前乗算)、stride = width * 4。
struct DecodedImage {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> pixels;

    size_t byteSize() const { return pixels.size(); }
};

// 画像デコーダのプラットフォーム抽象。Windows 実装は WIC (decoder_wic)。
// ImageCache のワーカースレッドから呼ばれるため、実装はスレッド安全にすること。
class IImageDecoder {
public:
    virtual ~IImageDecoder() = default;

    // 失敗時は nullptr を返す。
    virtual std::shared_ptr<DecodedImage> decode(const std::filesystem::path& path) = 0;
};

} // namespace blinker
