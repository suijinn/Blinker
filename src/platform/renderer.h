#pragma once

#include <cstdint>
#include <memory>

#include "core/geometry.h"
#include "platform/decoder.h"

namespace blinker {

// 描画のプラットフォーム抽象。Windows 実装は Direct2D (renderer_d2d)。
// UI スレッドからのみ呼ばれる。
class IRenderer {
public:
    virtual ~IRenderer() = default;

    virtual void resize(uint32_t width, uint32_t height) = 0;

    // image は nullptr 可(背景のみ描画)。zoom は補間モード選択のヒント。
    virtual void render(const std::shared_ptr<const DecodedImage>& image,
                        const Matrix3x2& imageToScreen, float zoom,
                        uint32_t backgroundRGB) = 0;
};

} // namespace blinker
