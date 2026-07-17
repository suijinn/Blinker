#pragma once

namespace blinker {

struct Point {
    float x = 0;
    float y = 0;
};

struct SizeF {
    float w = 0;
    float h = 0;
};

// 2D アフィン変換。行ベクトル規約 (p' = p * M) で、D2D1_MATRIX_3X2_F と同じ意味を持つ。
struct Matrix3x2 {
    float m11 = 1, m12 = 0;
    float m21 = 0, m22 = 1;
    float dx = 0, dy = 0;

    static Matrix3x2 identity() { return {}; }
    static Matrix3x2 translation(float x, float y) { return {1, 0, 0, 1, x, y}; }
    static Matrix3x2 scale(float s) { return {s, 0, 0, s, 0, 0}; }

    // 90 度単位の回転(スクリーン座標系=Y下向きで時計回り)。quarterTurns は任意の整数。
    static Matrix3x2 rotation90(int quarterTurns) {
        constexpr float kCos[4] = {1, 0, -1, 0};
        constexpr float kSin[4] = {0, 1, 0, -1};
        const int q = ((quarterTurns % 4) + 4) % 4;
        return {kCos[q], kSin[q], -kSin[q], kCos[q], 0, 0};
    }

    // this を適用してから b を適用する合成変換
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

    Point apply(Point p) const {
        return {p.x * m11 + p.y * m21 + dx, p.x * m12 + p.y * m22 + dy};
    }
};

} // namespace blinker
