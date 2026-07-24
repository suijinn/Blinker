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
     * @brief ファイル(実体)をクリップボードへ書き込む。
     *
     * "text/uri-list"(標準)と "x-special/gnome-copied-files"(Nautilus 等が
     * 貼り付けに使う)の 2 形式で file:// URI を書き込む。
     *
     * @param[in] paths 書き込むファイルのパス。相対パスは絶対パスへ解決される。
     * @return 書き込めたら true。空・SDL のエラー時は false。
     */
    bool setFiles(const std::vector<std::filesystem::path>& paths) override;

    /**
     * @brief クリップボードの "image/png" データを取得してデコードする。
     * @return 取得した画像(32bpp PBGRA)。画像がない・デコード失敗なら nullptr。
     */
    std::shared_ptr<DecodedImage> getImage() override;

    /**
     * @brief クリップボードのテキストを取得する。
     * @return 取得した文字列(UTF-8。CRLF は LF に正規化)。テキストがなければ空文字列。
     */
    std::string getText() override;
};

} // namespace blinker
