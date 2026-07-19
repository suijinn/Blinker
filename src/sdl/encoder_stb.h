#pragma once

#include <cstdint>
#include <vector>

#include "platform/encoder.h"

namespace blinker {

// stb_image_write によるエンコーダ(SDL バックエンド用)。
// PNG はストレートアルファへ戻して保存、JPEG/BMP は白背景に合成して保存する
// (EncoderWic と同じ扱い)。UI スレッドからのみ呼ばれる。
class EncoderStb final : public IImageEncoder {
public:
    bool encode(const DecodedImage& image, const std::filesystem::path& path) override;
};

// PNG をメモリへエンコードする(クリップボード用)。失敗時は空
std::vector<uint8_t> encodePngToMemory(const DecodedImage& image);

} // namespace blinker
