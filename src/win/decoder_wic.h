#pragma once

#include "platform/decoder.h"

/**
 * @file decoder_wic.h
 * @brief WIC による画像デコーダ(Windows 実装)。
 */

namespace blinker {

/**
 * @brief WIC による画像デコーダ。
 *
 * スレッドごとに COM/ファクトリを初期化するため、どのスレッドから呼んでもよい。
 */
class DecoderWic final : public IImageDecoder {
public:
    /**
     * @brief 画像ファイルを WIC でデコードする。
     * @param[in] path デコードする画像のパス。
     * @return デコード結果(32bpp PBGRA)。コーデックがない・不正データなら nullptr。
     */
    std::shared_ptr<DecodedImage> decode(const std::filesystem::path& path) override;
};

} // namespace blinker
