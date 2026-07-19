#pragma once

#include <memory>
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
    virtual bool setText(const std::string& text) = 0;  // UTF-8

    // クリップボードの画像を 32bpp PBGRA で取得する。画像がなければ nullptr
    virtual std::shared_ptr<DecodedImage> getImage() = 0;
};

} // namespace blinker
