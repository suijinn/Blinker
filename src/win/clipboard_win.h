#pragma once

#include <windows.h>

#include "platform/clipboard.h"

/**
 * @file clipboard_win.h
 * @brief Win32 クリップボード実装。
 */

namespace blinker {

/**
 * @brief Win32 クリップボード実装。
 *
 * 画像は CF_DIBV5(アルファ保持)と CF_DIB(24bpp、白背景に合成)の 2 形式で
 * 書き込み、アルファ非対応アプリでも自然に貼り付けられるようにする。
 * 読み取りは CF_DIBV5 優先で CF_DIB にフォールバック(変換は core/dib の imageFromDib)。
 */
class ClipboardWin final : public IClipboard {
public:
    /**
     * @brief クリップボード操作のオーナーウィンドウを設定する。
     * @param[in] hwnd OpenClipboard に渡すウィンドウハンドル。
     */
    void setOwner(HWND hwnd) { owner_ = hwnd; }

    /**
     * @brief 画像をクリップボードへ書き込む(CF_DIBV5 と CF_DIB の 2 形式)。
     * @param[in] image 書き込む画像(32bpp PBGRA)。
     * @return 書き込めたら true。クリップボードを開けない等の失敗時は false。
     */
    bool setImage(const DecodedImage& image) override;

    /**
     * @brief テキストをクリップボードへ書き込む(CF_UNICODETEXT)。
     * @param[in] text 書き込む文字列(UTF-8)。
     * @return 書き込めたら true。失敗時は false。
     */
    bool setText(const std::string& text) override;

    /**
     * @brief クリップボードの画像を取得する(CF_DIBV5 優先、CF_DIB へフォールバック)。
     * @return 取得した画像(32bpp PBGRA)。画像がない・非対応形式なら nullptr。
     */
    std::shared_ptr<DecodedImage> getImage() override;

private:
    HWND owner_ = nullptr;
};

} // namespace blinker
