#include "core/viewport.h"

#include <algorithm>

namespace blinker {

void Viewport::setWindowSize(SizeF size) {
    window_ = {std::max(size.w, 1.0f), std::max(size.h, 1.0f)};
    if (fitMode_) {
        recomputeFit();
    } else {
        clampPan();
    }
}

void Viewport::setImage(SizeF size) {
    image_ = size;
    quarterTurns_ = 0;
    pan_ = {};
    if (fitMode_) {
        recomputeFit();
    } else {
        clampPan();
    }
}

void Viewport::fit() {
    fitMode_ = true;
    recomputeFit();
}

void Viewport::actualSize() {
    fitMode_ = false;
    zoom_ = 1.0f;
    pan_ = {};
    clampPan();
}

void Viewport::zoomAt(float factor, Point screenPos) {
    if (image_.w <= 0 || image_.h <= 0) return;
    const float newZoom = std::clamp(zoom_ * factor, kMinZoom, kMaxZoom);
    factor = newZoom / zoom_;
    if (factor == 1.0f) return;
    // screenPos 直下の画像点を不動点に保つ。
    // 画像中心のスクリーン位置 C = ウィンドウ中心 + pan が (C - s) * factor + s へ移る。
    const float cx = window_.w * 0.5f + pan_.x;
    const float cy = window_.h * 0.5f + pan_.y;
    const float nx = (cx - screenPos.x) * factor + screenPos.x;
    const float ny = (cy - screenPos.y) * factor + screenPos.y;
    zoom_ = newZoom;
    pan_ = {nx - window_.w * 0.5f, ny - window_.h * 0.5f};
    fitMode_ = false;
    clampPan();
}

void Viewport::zoomStep(bool zoomIn) {
    zoomAt(zoomIn ? kZoomStep : 1.0f / kZoomStep, {window_.w * 0.5f, window_.h * 0.5f});
}

void Viewport::panBy(float dx, float dy) {
    pan_.x += dx;
    pan_.y += dy;
    clampPan();
}

void Viewport::rotate(int quarterTurnsDelta) {
    quarterTurns_ = ((quarterTurns_ + quarterTurnsDelta) % 4 + 4) % 4;
    pan_ = {};
    if (fitMode_) {
        recomputeFit();
    } else {
        clampPan();
    }
}

void Viewport::setFitUpscale(bool enable) {
    fitUpscale_ = enable;
    if (fitMode_) recomputeFit();
}

Matrix3x2 Viewport::imageToScreen() const {
    return Matrix3x2::translation(-image_.w * 0.5f, -image_.h * 0.5f)
         * Matrix3x2::rotation90(quarterTurns_)
         * Matrix3x2::scale(zoom_)
         * Matrix3x2::translation(window_.w * 0.5f + pan_.x, window_.h * 0.5f + pan_.y);
}

SizeF Viewport::rotatedImageSize() const {
    return (quarterTurns_ % 2 == 0) ? image_ : SizeF{image_.h, image_.w};
}

void Viewport::recomputeFit() {
    pan_ = {};
    const SizeF r = rotatedImageSize();
    if (r.w <= 0 || r.h <= 0) {
        zoom_ = 1.0f;
        return;
    }
    float z = std::min(window_.w / r.w, window_.h / r.h);
    if (!fitUpscale_) z = std::min(z, 1.0f);
    zoom_ = std::clamp(z, kMinZoom, kMaxZoom);
}

void Viewport::clampPan() {
    const SizeF r = rotatedImageSize();
    const float limitX = std::max(0.0f, (r.w * zoom_ - window_.w) * 0.5f);
    const float limitY = std::max(0.0f, (r.h * zoom_ - window_.h) * 0.5f);
    pan_.x = std::clamp(pan_.x, -limitX, limitX);
    pan_.y = std::clamp(pan_.y, -limitY, limitY);
}

} // namespace blinker
