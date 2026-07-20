#pragma once

#include <filesystem>

#include "platform/decoder.h"

/**
 * @file encoder.h
 * @brief 画像エンコーダのプラットフォーム抽象。
 */

namespace blinker {

/**
 * @brief 画像エンコーダのプラットフォーム抽象。
 *
 * Windows 実装は WIC (encoder_wic)、SDL バックエンドは stb_image_write (encoder_stb)。
 * UI スレッドからのみ呼ばれる(保存は同期処理)。
 */
class IImageEncoder {
public:
    virtual ~IImageEncoder() = default;

    /**
     * @brief 画像をファイルへ保存する。
     * @param[in] image 保存する画像(32bpp PBGRA)。
     * @param[in] path  保存先のパス。形式は拡張子で決まる (.png / .jpg / .jpeg / .bmp)。
     * @return 保存できたら true。非対応の拡張子・書き込み失敗なら false。
     */
    virtual bool encode(const DecodedImage& image, const std::filesystem::path& path) = 0;
};

} // namespace blinker
