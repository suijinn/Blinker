#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

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
     * @brief ファイル(実体)をクリップボードへ書き込む。
     *
     * エクスプローラ等のファイラに貼り付けるとファイルがコピーされる形式で書き込む
     * (Windows は CF_HDROP、SDL バックエンドは text/uri-list)。
     * 相対パスは実装側で絶対パスへ解決される。
     *
     * @param[in] paths 書き込むファイルのパス。空なら失敗扱い。
     * @return 書き込めたら true。失敗時は false。
     */
    virtual bool setFiles(const std::vector<std::filesystem::path>& paths) = 0;

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
