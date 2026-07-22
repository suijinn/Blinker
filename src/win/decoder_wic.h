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
     *
     * ファイル全体を一度メモリへ読んでから復号するため、デコード中に遅延 I/O が走らず、
     * ファイルを掴み続けることもない。EXIF 回転は core/exif の自前パーサで判定する。
     *
     * @param[in]  path  デコードする画像のパス。
     * @param[out] error 非 nullptr のとき、失敗時に「段階 (0xHRESULT)」形式の理由が入る。
     * @return デコード結果(32bpp PBGRA)。コーデックがない・不正データなら nullptr。
     */
    std::shared_ptr<DecodedImage> decode(const std::filesystem::path& path,
                                         std::string* error = nullptr) override;
};

} // namespace blinker
