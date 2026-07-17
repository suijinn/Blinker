#pragma once

#include "platform/encoder.h"

namespace blinker {

// WIC による画像保存。PNG はストレートアルファへ逆乗算して保持、
// JPEG / BMP はアルファ非対応のため白背景に合成した 24bpp で書き出す。
class EncoderWic final : public IImageEncoder {
public:
    bool encode(const DecodedImage& image, const std::filesystem::path& path) override;
};

} // namespace blinker
