#pragma once

#include "platform/encoder.h"

/**
 * @file encoder_wic.h
 * @brief WIC による画像保存(Windows 実装)。
 */

namespace blinker {

/**
 * @brief WIC による画像保存。
 *
 * PNG はストレートアルファへ逆乗算して保持、JPEG / BMP はアルファ非対応のため
 * 白背景に合成した 24bpp で書き出す。
 */
class EncoderWic final : public IImageEncoder {
public:
    /**
     * @brief 画像を WIC でファイルへ保存する。
     * @param[in] image 保存する画像(32bpp PBGRA)。
     * @param[in] path  保存先のパス。形式は拡張子で決まる (.png / .jpg / .jpeg / .bmp)。
     * @return 保存できたら true。非対応の拡張子・書き込み失敗なら false。
     */
    bool encode(const DecodedImage& image, const std::filesystem::path& path) override;
};

} // namespace blinker
