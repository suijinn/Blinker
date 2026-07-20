#pragma once

#include <cstdint>
#include <vector>

#include "platform/encoder.h"

/**
 * @file encoder_stb.h
 * @brief stb_image_write によるエンコーダ(SDL バックエンド用)。
 */

namespace blinker {

/**
 * @brief stb_image_write による画像保存。
 *
 * PNG はストレートアルファへ戻して保存、JPEG/BMP は白背景に合成して保存する
 * (EncoderWic と同じ扱い)。UI スレッドからのみ呼ばれる。
 */
class EncoderStb final : public IImageEncoder {
public:
    /**
     * @brief 画像を stb_image_write でファイルへ保存する。
     * @param[in] image 保存する画像(32bpp PBGRA)。
     * @param[in] path  保存先のパス。形式は拡張子で決まる (.png / .jpg / .jpeg / .bmp)。
     * @return 保存できたら true。非対応の拡張子・書き込み失敗なら false。
     */
    bool encode(const DecodedImage& image, const std::filesystem::path& path) override;
};

/**
 * @brief PNG をメモリへエンコードする(クリップボード用)。
 * @param[in] image エンコードする画像(32bpp PBGRA)。
 * @return PNG のバイト列。失敗時は空。
 */
std::vector<uint8_t> encodePngToMemory(const DecodedImage& image);

} // namespace blinker
