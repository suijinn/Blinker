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

    /**
     * @brief フレーム構成を調べる。
     *
     * stb_image はフレーム数だけを安く数える手段を持たないため、**拡張子が .gif なら
     * 「アニメーションかもしれない」として Animation を返す**(実際のフレーム数は
     * decodeAnimation が決め、1 枚しかなければ静止画のまま扱われる)。
     * 多ページ TIFF・ICO は stb_image が読めないので対象外。
     *
     * @param[in] path 調べる画像のパス。
     * @return GIF なら Animation、それ以外は Single。
     */
    SequenceInfo probeSequence(const std::filesystem::path& path) override;

    /**
     * @brief アニメーション GIF の全フレームをデコードする。
     *
     * stb_image の GIF ローダは Disposal の処理まで済ませた全画面のフレームを
     * まとめて返すので、AnimationCompositor は通さない。
     *
     * @param[in]  path   デコードする画像のパス。
     * @param[in]  limits 展開後の大きさとフレーム数の上限。
     * @param[out] out    成功時に全フレームと遅延時間が入る。
     * @param[out] error  非 nullptr のとき、失敗時に理由が入る。
     * @return 2 フレーム以上を展開できたら true。
     * @note stb_image は全フレームを 1 回の確保で返すため、**上限の判定は展開の後**に
     *       なる(確保そのものを事前に止められない)。SDL 版の既知の制限。
     */
    bool decodeAnimation(const std::filesystem::path& path, const AnimationLimits& limits,
                         ImageSequence& out, std::string* error) override;
};

/**
 * @brief メモリ上の画像データをデコードする(クリップボード用)。
 * @param[in] data 画像データの先頭を指すバッファ。
 * @param[in] size バッファのバイト数。
 * @return デコード結果(32bpp PBGRA)。非対応形式・不正データなら nullptr。
 */
std::shared_ptr<DecodedImage> decodeFromMemory(const uint8_t* data, size_t size);

} // namespace blinker
