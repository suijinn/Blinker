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
 * @todo EXIF 回転に未対応(stb_image が EXIF を読まないため)。スマートフォンで
 *       撮影した JPEG が横倒しで表示される。EXIF Orientation を自前で読んで
 *       core/exif の `applyExifOrientation` に渡す必要がある。
 */
class DecoderStb final : public IImageDecoder {
public:
    /**
     * @brief 画像ファイルを stb_image でデコードする。
     * @param[in]  path  デコードする画像のパス。
     * @param[out] error 非 nullptr のとき、失敗時に stb_image の失敗理由が入る。
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
