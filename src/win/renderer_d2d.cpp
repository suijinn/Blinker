#include "win/renderer_d2d.h"

#include <algorithm>

namespace blinker {
namespace {

using Microsoft::WRL::ComPtr;

constexpr size_t kBitmapCacheSize = 3;

D2D1_COLOR_F colorFromRGB(uint32_t rgb) {
    return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f,
                        (rgb & 0xFF) / 255.0f);
}

} // namespace

RendererD2D::RendererD2D(HWND hwnd) : hwnd_(hwnd) {
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory_.GetAddressOf());
}

bool RendererD2D::ensureTarget() {
    if (target_) return true;
    if (!factory_) return false;
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const D2D1_SIZE_U size = D2D1::SizeU(std::max<LONG>(rc.right - rc.left, 1),
                                         std::max<LONG>(rc.bottom - rc.top, 1));
    // DPI を 96 に固定し、DIP = 物理ピクセルとして扱う(座標系を全編ピクセルで統一)
    const auto props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.0f, 96.0f);
    return SUCCEEDED(factory_->CreateHwndRenderTarget(
        props, D2D1::HwndRenderTargetProperties(hwnd_, size), &target_));
}

void RendererD2D::discardTarget() {
    bitmaps_.clear();
    target_.Reset();
}

void RendererD2D::resize(uint32_t width, uint32_t height) {
    if (target_) {
        target_->Resize(D2D1::SizeU(std::max(width, 1u), std::max(height, 1u)));
    }
}

ID2D1Bitmap* RendererD2D::bitmapFor(const std::shared_ptr<const DecodedImage>& image) {
    for (auto it = bitmaps_.begin(); it != bitmaps_.end(); ++it) {
        if (it->first == image) {
            bitmaps_.splice(bitmaps_.begin(), bitmaps_, it);
            return bitmaps_.front().second.Get();
        }
    }
    ComPtr<ID2D1Bitmap> bitmap;
    const auto props = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(target_->CreateBitmap(D2D1::SizeU(image->width, image->height),
                                     image->pixels.data(), image->width * 4, props, &bitmap))) {
        return nullptr;
    }
    bitmaps_.emplace_front(image, std::move(bitmap));
    if (bitmaps_.size() > kBitmapCacheSize) bitmaps_.pop_back();
    return bitmaps_.front().second.Get();
}

void RendererD2D::render(const std::shared_ptr<const DecodedImage>& image,
                         const Matrix3x2& imageToScreen, float zoom, uint32_t backgroundRGB) {
    if (!ensureTarget()) return;
    target_->BeginDraw();
    target_->Clear(colorFromRGB(backgroundRGB));
    if (image && image->width > 0) {
        if (ID2D1Bitmap* bitmap = bitmapFor(image)) {
            target_->SetTransform(D2D1::Matrix3x2F(imageToScreen.m11, imageToScreen.m12,
                                                   imageToScreen.m21, imageToScreen.m22,
                                                   imageToScreen.dx, imageToScreen.dy));
            // 拡大表示時はピクセル境界が見えるよう最近傍補間へ切り替える
            const auto mode = zoom >= 4.0f ? D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR
                                           : D2D1_BITMAP_INTERPOLATION_MODE_LINEAR;
            const auto rect = D2D1::RectF(0, 0, static_cast<float>(image->width),
                                          static_cast<float>(image->height));
            target_->DrawBitmap(bitmap, rect, 1.0f, mode, rect);
            target_->SetTransform(D2D1::Matrix3x2F::Identity());
        }
    }
    if (target_->EndDraw() == D2DERR_RECREATE_TARGET) {
        discardTarget();  // 次回の render で作り直す
    }
}

} // namespace blinker
