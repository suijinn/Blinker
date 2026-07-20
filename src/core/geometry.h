#pragma once

/**
 * @file geometry.h
 * @brief 座標・サイズ・2D アフィン変換の値型。
 *
 * 座標はすべて物理ピクセル(D2D は DPI 96 固定のため DIP = px)。
 */

namespace blinker {

/// @brief 2D の点(物理ピクセル)。
struct Point {
    float x = 0;  ///< X 座標
    float y = 0;  ///< Y 座標
};

/// @brief 2D のサイズ(物理ピクセル)。
struct SizeF {
    float w = 0;  ///< 幅
    float h = 0;  ///< 高さ
};

/**
 * @brief 2D アフィン変換。
 *
 * 行ベクトル規約 (p' = p * M) で、D2D1_MATRIX_3X2_F と同じ意味を持つ。
 */
struct Matrix3x2 {
    float m11 = 1;  ///< X 軸の変換係数(X 成分)。スケール・回転を担う
    float m12 = 0;  ///< X 軸の変換係数(Y 成分)。せん断・回転を担う
    float m21 = 0;  ///< Y 軸の変換係数(X 成分)。せん断・回転を担う
    float m22 = 1;  ///< Y 軸の変換係数(Y 成分)。スケール・回転を担う
    float dx = 0;   ///< X 方向の平行移動量
    float dy = 0;   ///< Y 方向の平行移動量

    /**
     * @brief 単位行列を返す。
     * @return 何も変換しない行列。
     */
    static Matrix3x2 identity() { return {}; }

    /**
     * @brief 平行移動行列を作る。
     * @param[in] x X 方向の移動量。
     * @param[in] y Y 方向の移動量。
     * @return 平行移動を表す行列。
     */
    static Matrix3x2 translation(float x, float y) { return {1, 0, 0, 1, x, y}; }

    /**
     * @brief 原点中心の等方スケール行列を作る。
     * @param[in] s 拡大率。
     * @return スケールを表す行列。
     */
    static Matrix3x2 scale(float s) { return {s, 0, 0, s, 0, 0}; }

    /**
     * @brief 90 度単位の回転行列を作る(スクリーン座標系 = Y 下向きで時計回り)。
     * @param[in] quarterTurns 90 度回転の回数。負値・4 以上を含む任意の整数を受け付ける。
     * @return 回転を表す行列。
     */
    static Matrix3x2 rotation90(int quarterTurns) {
        constexpr float kCos[4] = {1, 0, -1, 0};
        constexpr float kSin[4] = {0, 1, 0, -1};
        const int q = ((quarterTurns % 4) + 4) % 4;
        return {kCos[q], kSin[q], -kSin[q], kCos[q], 0, 0};
    }

    /**
     * @brief 合成変換(this を適用してから b を適用する)。
     * @param[in] b 後段に適用する行列。
     * @return 合成された行列。
     */
    Matrix3x2 operator*(const Matrix3x2& b) const {
        return {
            m11 * b.m11 + m12 * b.m21,
            m11 * b.m12 + m12 * b.m22,
            m21 * b.m11 + m22 * b.m21,
            m21 * b.m12 + m22 * b.m22,
            dx * b.m11 + dy * b.m21 + b.dx,
            dx * b.m12 + dy * b.m22 + b.dy,
        };
    }

    /**
     * @brief 点にこの変換を適用する。
     * @param[in] p 変換前の点。
     * @return 変換後の点。
     */
    Point apply(Point p) const {
        return {p.x * m11 + p.y * m21 + dx, p.x * m12 + p.y * m22 + dy};
    }

    /**
     * @brief 逆変換を求める。
     * @return 逆行列。特異(行列式が 0)の場合は単位行列。
     */
    Matrix3x2 inverted() const {
        const float det = m11 * m22 - m12 * m21;
        if (det == 0) return {};
        const float inv = 1.0f / det;
        return {
            m22 * inv,
            -m12 * inv,
            -m21 * inv,
            m11 * inv,
            (m21 * dy - m22 * dx) * inv,
            (m12 * dx - m11 * dy) * inv,
        };
    }
};

} // namespace blinker
