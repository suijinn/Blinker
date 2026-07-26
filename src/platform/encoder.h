#pragma once

#include <filesystem>

#include "platform/decoder.h"

/**
 * @file encoder.h
 * @brief 画像エンコーダのプラットフォーム抽象。
 */

namespace blinker {

/**
 * @brief 保存時のエンコード設定。
 *
 * 形式に依らない設定だけを持つ(現状は JPEG 品質のみ)。
 * 値は blinker.ini の `[save]` に由来する。
 */
struct EncodeOptions {
    /// JPEG の品質 (1-100)。他の形式では無視される
    int jpegQuality = 90;
};

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
     * @brief 拡張子から保存できる形式かを判定する。
     *
     * 上書き保存の可否を、ファイルへ手を付ける前に知るためのもの
     * (対応形式は実装ごとに違うので core 側では判定しない)。
     *
     * @param[in] path 判定するパス。実在しなくてもよい(拡張子だけを見る)。
     * @return 保存できる拡張子なら true。大文字小文字は区別しない。
     */
    virtual bool supports(const std::filesystem::path& path) const = 0;

    /**
     * @brief 画像をファイルへ保存する。
     * @param[in] image   保存する画像(32bpp PBGRA)。
     * @param[in] path    保存先のパス。形式は拡張子で決まる (.png / .jpg / .jpeg / .bmp)。
     * @param[in] options エンコード設定(JPEG 品質など)。
     * @return 保存できたら true。非対応の拡張子・書き込み失敗なら false。
     */
    virtual bool encode(const DecodedImage& image, const std::filesystem::path& path,
                        const EncodeOptions& options) = 0;
};

} // namespace blinker
