#pragma once

#include "core/geometry.h"

/**
 * @file viewport.h
 * @brief 画像 → スクリーンの表示変換(ズーム/パン/フィット/回転)。
 */

namespace blinker {

/**
 * @brief 画像 → スクリーンの表示変換を管理する。
 *
 * 座標はすべて物理ピクセル。パンは「ウィンドウ中心からの画像中心のオフセット」として保持する。
 */
class Viewport {
public:
    static constexpr float kMinZoom = 0.01f;  ///< ズーム倍率の下限
    static constexpr float kMaxZoom = 64.0f;  ///< ズーム倍率の上限
    static constexpr float kZoomStep = 1.25f; ///< zoomStep 1 回あたりの倍率

    /**
     * @brief ウィンドウ(ビューポート)サイズを設定する。
     * @param[in] size 新しいサイズ(物理ピクセル)。フィットモードなら再フィットする。
     */
    void setWindowSize(SizeF size);

    /**
     * @brief 表示する画像のサイズを設定する(画像切替)。
     * @param[in] size 画像のサイズ(ピクセル)。回転をリセットし、フィットモードなら再フィットする。
     */
    void setImage(SizeF size);

    /// @brief ウィンドウにフィットさせる(フィットモード ON)。
    void fit();

    /// @brief 等倍(ズーム 1.0)で表示する(フィットモード OFF)。
    void actualSize();

    /**
     * @brief 指定位置を不動点にしてズームする。
     * @param[in] factor    現在のズームに掛ける倍率。1 より大きければ拡大。
     * @param[in] screenPos 不動点にするスクリーン座標。直下の画像点が動かない。
     */
    void zoomAt(float factor, Point screenPos);

    /**
     * @brief ウィンドウ中心を基準に 1 段ズームする。
     * @param[in] zoomIn true で拡大、false で縮小。
     */
    void zoomStep(bool zoomIn);

    /**
     * @brief 表示位置をずらす。
     * @param[in] dx X 方向の移動量(スクリーン px)。
     * @param[in] dy Y 方向の移動量(スクリーン px)。
     */
    void panBy(float dx, float dy);

    /**
     * @brief 90 度単位で回転する。
     * @param[in] quarterTurnsDelta 回転量。+1 で時計回りに 90 度。
     */
    void rotate(int quarterTurnsDelta);

    /**
     * @brief 画像座標 → スクリーン座標の変換行列を返す。
     * @return ズーム・パン・回転を合成した行列。
     */
    Matrix3x2 imageToScreen() const;

    /**
     * @brief スクリーン座標 → 画像座標の変換行列を返す。
     * @return imageToScreen() の逆行列。
     */
    Matrix3x2 screenToImage() const { return imageToScreen().inverted(); }

    /**
     * @brief 現在のズーム倍率を返す。
     * @return ズーム倍率(1.0 が等倍)。
     */
    float zoom() const { return zoom_; }

    /**
     * @brief フィットモードかを返す。
     * @return フィットモードなら true。
     */
    bool fitMode() const { return fitMode_; }

    /**
     * @brief 現在の回転角を返す。
     * @return 回転角(度)。90 の倍数。
     */
    int rotationDegrees() const { return quarterTurns_ * 90; }

    /**
     * @brief フィット時に等倍を超えて拡大するかを設定する。
     * @param[in] enable true なら小さい画像もウィンドウいっぱいに拡大する。
     */
    void setFitUpscale(bool enable);

private:
    /**
     * @brief 回転を反映した画像サイズを返す。
     * @return 90/270 度回転時は幅と高さを入れ替えたサイズ。
     */
    SizeF rotatedImageSize() const;

    /// @brief フィットモード時のズーム倍率を計算し直す。
    void recomputeFit();

    /// @brief 画像がウィンドウから離れすぎないようパン量を制限する。
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
