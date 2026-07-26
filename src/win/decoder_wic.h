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
     * @brief カラーマネジメントの有無を指定して構築する。
     * @param[in] colorManagement true なら埋め込みプロファイルを sRGB へ変換する
     *                            (`[view] color_management`)。
     */
    explicit DecoderWic(bool colorManagement = true) : colorManagement_(colorManagement) {}

    /**
     * @brief 画像ファイルを WIC でデコードする。
     *
     * EXIF 回転は core/exif の自前パーサで判定して復号後に適用する。
     * 上限を超える大きさの画像は縮小して取り込み、元の大きさを
     * `DecodedImage::sourceWidth/sourceHeight` に残す。
     * **色変換はここでは行わない**。カラーマネジメントが有効で変換できる
     * プロファイルが付いている場合は `DecodedImage::colorPending` を立てるだけで、
     * 実際の変換は decodeColorManaged による読み直しに任せる(表示を待たせないため)。
     *
     * @param[in]  path  デコードする画像のパス。
     * @param[out] error 非 nullptr のとき、失敗時に「段階 (0xHRESULT)」形式の理由が入る。
     * @return デコード結果(32bpp PBGRA)。コーデックがない・不正データなら nullptr。
     */
    std::shared_ptr<DecodedImage> decode(const std::filesystem::path& path,
                                         std::string* error = nullptr) override;

    /**
     * @brief 埋め込みプロファイルを sRGB へ変換して読み直す。
     * @param[in]  path  読み直す画像のパス。
     * @param[out] error 非 nullptr のとき、失敗時に理由が入る。
     * @return 変換済み画像(`colorConverted` が立つ)。カラーマネジメントが無効・
     *         プロファイルが無い・WIC が変換できない場合は nullptr。
     */
    std::shared_ptr<DecodedImage> decodeColorManaged(const std::filesystem::path& path,
                                                     std::string* error = nullptr) override;

private:
    /**
     * @brief decode / decodeColorManaged の共通処理。
     * @param[in]  path                デコードする画像のパス。
     * @param[out] error               非 nullptr のとき、失敗時に理由が入る。
     * @param[in]  applyColorTransform true なら sRGB への色変換まで行う。
     * @return デコード結果(32bpp PBGRA)。失敗時は nullptr。
     */
    std::shared_ptr<DecodedImage> decodeInternal(const std::filesystem::path& path,
                                                 std::string* error, bool applyColorTransform);

    bool colorManagement_ = true;
};

} // namespace blinker
