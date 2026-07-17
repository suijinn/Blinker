#pragma once

#include "core/geometry.h"

namespace blinker {

// 画像→スクリーンの表示変換(ズーム/パン/フィット/回転)を管理する。
// 座標はすべて物理ピクセル。パンは「ウィンドウ中心からの画像中心のオフセット」。
class Viewport {
public:
    static constexpr float kMinZoom = 0.01f;
    static constexpr float kMaxZoom = 64.0f;
    static constexpr float kZoomStep = 1.25f;

    void setWindowSize(SizeF size);
    void setImage(SizeF size);                   // 画像切替。回転をリセットし、フィットモードなら再フィット
    void fit();                                  // ウィンドウにフィット(フィットモードON)
    void actualSize();                           // 等倍表示
    void zoomAt(float factor, Point screenPos);  // screenPos 直下の画像点を不動点にズーム
    void zoomStep(bool zoomIn);                  // ウィンドウ中心基準
    void panBy(float dx, float dy);
    void rotate(int quarterTurnsDelta);          // 90度単位(+1で時計回り)

    Matrix3x2 imageToScreen() const;
    float zoom() const { return zoom_; }
    bool fitMode() const { return fitMode_; }
    int rotationDegrees() const { return quarterTurns_ * 90; }
    void setFitUpscale(bool enable);             // フィット時に等倍を超えて拡大するか

private:
    SizeF rotatedImageSize() const;
    void recomputeFit();
    void clampPan();

    SizeF window_{1, 1};
    SizeF image_{0, 0};
    float zoom_ = 1.0f;
    Point pan_{};
    int quarterTurns_ = 0;
    bool fitMode_ = true;
    bool fitUpscale_ = false;
};

} // namespace blinker
