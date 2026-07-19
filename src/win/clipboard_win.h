#pragma once

#include <windows.h>

#include "platform/clipboard.h"

namespace blinker {

// Win32 クリップボード実装。画像は CF_DIBV5(アルファ保持)と CF_DIB(24bpp、
// 白背景に合成)の2形式で書き込み、アルファ非対応アプリでも自然に貼り付けられるようにする。
// 読み取りは CF_DIBV5 優先で CF_DIB にフォールバック(変換は core/dib の imageFromDib)。
class ClipboardWin final : public IClipboard {
public:
    void setOwner(HWND hwnd) { owner_ = hwnd; }

    bool setImage(const DecodedImage& image) override;
    bool setText(const std::string& text) override;
    std::shared_ptr<DecodedImage> getImage() override;

private:
    HWND owner_ = nullptr;
};

} // namespace blinker
