#pragma once

#include <string>

#include "platform/decoder.h"

namespace blinker {

// クリップボードのプラットフォーム抽象。Windows 実装は clipboard_win。
// UI スレッドからのみ呼ばれる。
class IClipboard {
public:
    virtual ~IClipboard() = default;

    // 画像を書き込む(image のピクセルは 32bpp PBGRA)。失敗時 false
    virtual bool setImage(const DecodedImage& image) = 0;
    virtual bool setText(const std::wstring& text) = 0;
};

} // namespace blinker
