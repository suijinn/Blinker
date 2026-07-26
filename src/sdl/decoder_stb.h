#pragma once

#include "platform/decoder.h"

/**
 * @file decoder_stb.h
 * @brief stb_image によるデコーダ(SDL バックエンド用)。
 */

namespace blinker {

/**
 * @brief stb_image による画像デコーダ。
 *
 * JPEG/PNG/BMP/GIF/TGA/PSD 等に対応し、32bpp PBGRA へ統一する。
 * 状態を持たないためスレッド安全(ワーカースレッドから呼ばれる)。
 *
 * stb_image は EXIF を読まないため、向きは core/exif の `readExifOrientation` で
 * 自前に解析して `applyExifOrientation` で適用する(JPEG の APP1 と PNG の eXIf)。
 */
class DecoderStb final : public IImageDecoder {
public:
    /**
     * @brief 画像ファイルを stb_image でデコードし、EXIF Orientation を適用する。
     * @param[in]  path  デコードする画像のパス。
     * @param[out] error 非 nullptr のとき、失敗時に理由が入る(読み込み失敗、
     *                   または stb_image の失敗理由)。
     * @return デコード結果(32bpp PBGRA)。非対応形式・不正データなら nullptr。
     */
    std::shared_ptr<DecodedImage> decode(const std::filesystem::path& path,
                                         std::string* error = nullptr) override;
};

/**
 * @brief メモリ上の画像データをデコードする(クリップボード用)。
 * @param[in] data 画像データの先頭を指すバッファ。
 * @param[in] size バッファのバイト数。
 * @return デコード結果(32bpp PBGRA)。非対応形式・不正データなら nullptr。
 */
std::shared_ptr<DecodedImage> decodeFromMemory(const uint8_t* data, size_t size);

} // namespace blinker
