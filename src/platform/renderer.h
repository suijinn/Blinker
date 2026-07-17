#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "core/geometry.h"
#include "platform/decoder.h"

namespace blinker {

// ステータスバーの描画内容。App が文字列まで組み立て、レンダラは描くだけ。
struct StatusBarView {
    bool visible = false;
    float height = 0;
    uint32_t backgroundRGB = 0;
    uint32_t textRGB = 0;
    std::wstring leftText;   // 通知メッセージ or 画像情報
    std::wstring rightText;  // カーソル位置の座標と色
};

// 描画のプラットフォーム抽象。Windows 実装は Direct2D (renderer_d2d)。
// UI スレッドからのみ呼ばれる。
class IRenderer {
public:
    virtual ~IRenderer() = default;

    virtual void resize(uint32_t width, uint32_t height) = 0;

    // image は nullptr 可(背景のみ描画)。zoom は補間モード選択のヒント。
    virtual void render(const std::shared_ptr<const DecodedImage>& image,
                        const Matrix3x2& imageToScreen, float zoom, uint32_t backgroundRGB,
                        const StatusBarView& statusBar) = 0;
};

} // namespace blinker
