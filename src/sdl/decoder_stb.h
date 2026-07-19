#pragma once

#include "platform/decoder.h"

namespace blinker {

// stb_image によるデコーダ(SDL バックエンド用)。JPEG/PNG/BMP/GIF/TGA/PSD 等に対応。
// 32bpp PBGRA へ統一する。状態を持たないためスレッド安全(ワーカースレッドから呼ばれる)。
// 制限: EXIF 回転は適用されない(stb_image が EXIF を読まないため)。
class DecoderStb final : public IImageDecoder {
public:
    std::shared_ptr<DecodedImage> decode(const std::filesystem::path& path) override;
};

// メモリ上の画像データをデコードする(クリップボード用)
std::shared_ptr<DecodedImage> decodeFromMemory(const uint8_t* data, size_t size);

} // namespace blinker
