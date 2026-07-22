#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

/**
 * @file decoder.h
 * @brief デコード済み画像の表現と、画像デコーダのプラットフォーム抽象。
 */

namespace blinker {

/**
 * @brief デコード済み画像。
 *
 * ピクセルは 32bit BGRA(アルファ事前乗算)、stride = width * 4。
 */
struct DecodedImage {
    uint32_t width = 0;            ///< 幅(ピクセル)
    uint32_t height = 0;           ///< 高さ(ピクセル)
    std::vector<uint8_t> pixels;   ///< ピクセルデータ(32bpp PBGRA、上から下へ)

    /**
     * @brief ピクセルデータのバイト数を返す。
     * @return pixels のサイズ(バイト)。
     */
    size_t byteSize() const { return pixels.size(); }
};

/**
 * @brief 画像デコーダのプラットフォーム抽象。
 *
 * Windows 実装は WIC (decoder_wic)、SDL バックエンドは stb_image (decoder_stb)。
 * ImageCache のワーカースレッドから呼ばれるため、実装はスレッド安全にすること。
 */
class IImageDecoder {
public:
    virtual ~IImageDecoder() = default;

    /**
     * @brief 画像ファイルをデコードする。
     * @param[in]  path  デコードする画像のパス。
     * @param[out] error 非 nullptr のとき、失敗した場合に限り原因を表す短い UTF-8 文字列
     *                   (失敗した段階とプラットフォーム固有のエラーコード。例:
     *                   `"PBGRA変換 (0x88982F50)"`)が入る。成功時は変更しない。
     *                   ステータスバーへそのまま表示される想定。
     * @return デコード結果(32bpp PBGRA)。失敗時は nullptr。
     */
    virtual std::shared_ptr<DecodedImage> decode(const std::filesystem::path& path,
                                                 std::string* error = nullptr) = 0;
};

} // namespace blinker
