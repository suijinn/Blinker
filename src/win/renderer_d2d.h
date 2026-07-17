#pragma once

#include <windows.h>

#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <list>
#include <utility>

#include "platform/renderer.h"

namespace blinker {

// ID2D1HwndRenderTarget による描画。文字(ステータスバー)は DirectWrite。UI スレッド専用。
class RendererD2D final : public IRenderer {
public:
    explicit RendererD2D(HWND hwnd);

    void resize(uint32_t width, uint32_t height) override;
    void render(const std::shared_ptr<const DecodedImage>& image,
                const Matrix3x2& imageToScreen, float zoom, uint32_t backgroundRGB,
                const StatusBarView& statusBar) override;

private:
    bool ensureTarget();
    void discardTarget();
    ID2D1Bitmap* bitmapFor(const std::shared_ptr<const DecodedImage>& image);
    void drawStatusBar(const StatusBarView& bar);

    HWND hwnd_;
    Microsoft::WRL::ComPtr<ID2D1Factory> factory_;
    Microsoft::WRL::ComPtr<IDWriteFactory> dwriteFactory_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> textFormat_;
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> target_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_;  // target_ と同寿命
    // 直近使用したデコード画像の GPU ビットマップ(小さな LRU)。
    // shared_ptr をキーに持つことで CPU 側画像の解放とアドレス再利用による取り違えを防ぐ
    std::list<std::pair<std::shared_ptr<const DecodedImage>, Microsoft::WRL::ComPtr<ID2D1Bitmap>>>
        bitmaps_;
};

} // namespace blinker
