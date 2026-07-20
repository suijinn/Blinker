#pragma once

#include <memory>
#include <string>

#include "platform/decoder.h"

/**
 * @file clipboard.h
 * @brief クリップボードのプラットフォーム抽象。
 */

namespace blinker {

/**
 * @brief クリップボードのプラットフォーム抽象。
 *
 * Windows 実装は clipboard_win、SDL バックエンドは clipboard_sdl。
 * UI スレッドからのみ呼ばれる。
 */
class IClipboard {
public:
    virtual ~IClipboard() = default;

    /**
     * @brief 画像をクリップボードへ書き込む。
     * @param[in] image 書き込む画像(32bpp PBGRA)。
     * @return 書き込めたら true。失敗時は false。
     */
    virtual bool setImage(const DecodedImage& image) = 0;

    /**
     * @brief テキストをクリップボードへ書き込む。
     * @param[in] text 書き込む文字列(UTF-8)。
     * @return 書き込めたら true。失敗時は false。
     */
    virtual bool setText(const std::string& text) = 0;

    /**
     * @brief クリップボードの画像を取得する。
     * @return 取得した画像(32bpp PBGRA)。画像がなければ nullptr。
     */
    virtual std::shared_ptr<DecodedImage> getImage() = 0;

    /**
     * @brief クリップボードのテキストを取得する(テキスト注釈への貼り付け用)。
     * @return 取得した文字列(UTF-8。改行は LF に正規化される)。
     *         テキストがなければ空文字列。
     */
    virtual std::string getText() = 0;
};

} // namespace blinker
