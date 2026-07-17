#pragma once

#include <windows.h>

#include <d2d1.h>
#include <wrl/client.h>

#include <list>
#include <utility>

#include "platform/renderer.h"

namespace blinker {

// ID2D1HwndRenderTarget による描画。UI スレッド専用。
class RendererD2D final : public IRenderer {
public:
    explicit RendererD2D(HWND hwnd);

    void resize(uint32_t width, uint32_t height) override;
    void render(const std::shared_ptr<const DecodedImage>& image,
                const Matrix3x2& imageToScreen, float zoom, uint32_t backgroundRGB) override;

private:
    bool ensureTarget();
    void discardTarget();
    ID2D1Bitmap* bitmapFor(const std::shared_ptr<const DecodedImage>& image);

    HWND hwnd_;
    Microsoft::WRL::ComPtr<ID2D1Factory> factory_;
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> target_;
    // 直近使用したデコード画像の GPU ビットマップ(小さな LRU)。
    // shared_ptr をキーに持つことで CPU 側画像の解放とアドレス再利用による取り違えを防ぐ
    std::list<std::pair<std::shared_ptr<const DecodedImage>, Microsoft::WRL::ComPtr<ID2D1Bitmap>>>
        bitmaps_;
};

} // namespace blinker
