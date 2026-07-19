#pragma once

#include "platform/clipboard.h"

namespace blinker {

// SDL3 クリップボード実装。テキストは SDL_SetClipboardText、画像は
// "image/png" MIME で PNG バイト列を受け渡しする(GNOME/KDE の慣習に合わせる)。
// UI スレッドからのみ呼ばれる。
class ClipboardSdl final : public IClipboard {
public:
    bool setImage(const DecodedImage& image) override;
    bool setText(const std::string& text) override;
    std::shared_ptr<DecodedImage> getImage() override;
};

} // namespace blinker
