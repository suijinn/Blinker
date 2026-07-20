#pragma once

#include "platform/clipboard.h"

/**
 * @file clipboard_sdl.h
 * @brief SDL3 クリップボード実装。
 */

namespace blinker {

/**
 * @brief SDL3 クリップボード実装。
 *
 * テキストは SDL_SetClipboardText、画像は "image/png" MIME で PNG バイト列を
 * 受け渡しする(GNOME/KDE の慣習に合わせる)。UI スレッドからのみ呼ばれる。
 */
class ClipboardSdl final : public IClipboard {
public:
    /**
     * @brief 画像を PNG バイト列としてクリップボードへ書き込む。
     * @param[in] image 書き込む画像(32bpp PBGRA)。
     * @return 書き込めたら true。エンコード失敗・SDL のエラー時は false。
     */
    bool setImage(const DecodedImage& image) override;

    /**
     * @brief テキストをクリップボードへ書き込む。
     * @param[in] text 書き込む文字列(UTF-8)。
     * @return 書き込めたら true。失敗時は false。
     */
    bool setText(const std::string& text) override;

    /**
     * @brief クリップボードの "image/png" データを取得してデコードする。
     * @return 取得した画像(32bpp PBGRA)。画像がない・デコード失敗なら nullptr。
     */
    std::shared_ptr<DecodedImage> getImage() override;
};

} // namespace blinker
