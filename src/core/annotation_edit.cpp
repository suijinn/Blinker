#include "core/annotation_edit.h"

#include <algorithm>
#include <cmath>

namespace blinker {
namespace {

constexpr float kPi = 3.14159265358979323846f;

// center 周りに deg 回転(スクリーン座標系=Y下向きで時計回り。D2D の Rotation と同じ)
Point rotateAround(Point p, Point center, float deg) {
    const float rad = deg * kPi / 180.0f;
    const float c = std::cos(rad);
    const float s = std::sin(rad);
    const float dx = p.x - center.x;
    const float dy = p.y - center.y;
    return {center.x + dx * c - dy * s, center.y + dx * s + dy * c};
}

float pointSegmentDistance(Point p, Point a, Point b) {
    const float abx = b.x - a.x;
    const float aby = b.y - a.y;
    const float lenSq = abx * abx + aby * aby;
    float t = lenSq > 0 ? ((p.x - a.x) * abx + (p.y - a.y) * aby) / lenSq : 0.0f;
    t = std::clamp(t, 0.0f, 1.0f);
    const float dx = p.x - (a.x + abx * t);
    const float dy = p.y - (a.y + aby * t);
    return std::sqrt(dx * dx + dy * dy);
}

} // namespace

BoundsF annotationBounds(const AnnotationSpec& spec) {
    return {std::min(spec.p1.x, spec.p2.x), std::min(spec.p1.y, spec.p2.y),
            std::max(spec.p1.x, spec.p2.x), std::max(spec.p1.y, spec.p2.y)};
}

Point annotationCenter(const AnnotationSpec& spec) {
    const BoundsF b = annotationBounds(spec);
    return {(b.minX + b.maxX) * 0.5f, (b.minY + b.maxY) * 0.5f};
}

std::array<Point, 4> rotatedCorners(const AnnotationSpec& spec) {
    const BoundsF b = annotationBounds(spec);
    const Point c = annotationCenter(spec);
    std::array<Point, 4> corners{Point{b.minX, b.minY}, Point{b.maxX, b.minY},
                                 Point{b.maxX, b.maxY}, Point{b.minX, b.maxY}};
    if (spec.angleDeg != 0) {
        for (Point& p : corners) p = rotateAround(p, c, spec.angleDeg);
    }
    return corners;
}

bool hitTestAnnotation(const AnnotationSpec& spec, Point imagePos, float tolerance) {
    const BoundsF b = annotationBounds(spec);
    const Point c = annotationCenter(spec);
    // 回転は bbox 中心周りなので、点を逆回転してから無回転の形状と比較する
    const Point q = spec.angleDeg != 0 ? rotateAround(imagePos, c, -spec.angleDeg) : imagePos;
    const float reach = spec.strokeWidth * 0.5f + tolerance;
    switch (spec.kind) {
    case AnnotationSpec::Kind::Rect: {
        const bool insideOuter = q.x >= b.minX - reach && q.x <= b.maxX + reach &&
                                 q.y >= b.minY - reach && q.y <= b.maxY + reach;
        const bool insideInner = q.x > b.minX + reach && q.x < b.maxX - reach &&
                                 q.y > b.minY + reach && q.y < b.maxY - reach;
        return insideOuter && !insideInner;
    }
    case AnnotationSpec::Kind::Ellipse: {
        const float rx = (b.maxX - b.minX) * 0.5f;
        const float ry = (b.maxY - b.minY) * 0.5f;
        // 半径を reach ぶん拡縮した2つの楕円に挟まれたリング内なら輪郭上とみなす
        const auto inside = [&q, &c](float ex, float ey) {
            if (ex <= 0 || ey <= 0) return false;
            const float nx = (q.x - c.x) / ex;
            const float ny = (q.y - c.y) / ey;
            return nx * nx + ny * ny <= 1.0f;
        };
        return inside(rx + reach, ry + reach) && !inside(rx - reach, ry - reach);
    }
    case AnnotationSpec::Kind::Line:
    case AnnotationSpec::Kind::Arrow:
        return pointSegmentDistance(q, spec.p1, spec.p2) <= reach;
    case AnnotationSpec::Kind::Text:
        return q.x >= b.minX - tolerance && q.x <= b.maxX + tolerance &&
               q.y >= b.minY - tolerance && q.y <= b.maxY + tolerance;
    }
    return false;
}

std::optional<size_t> hitTestAnnotations(const std::vector<AnnotationSpec>& specs,
                                         Point imagePos, float tolerance) {
    for (size_t i = specs.size(); i > 0; --i) {
        if (hitTestAnnotation(specs[i - 1], imagePos, tolerance)) return i - 1;
    }
    return std::nullopt;
}

void translateAnnotation(AnnotationSpec& spec, float dx, float dy) {
    spec.p1.x += dx;
    spec.p1.y += dy;
    spec.p2.x += dx;
    spec.p2.y += dy;
}

Point rotationHandlePos(const AnnotationSpec& spec, const Matrix3x2& imageToScreen,
                        float offsetPx) {
    const auto corners = rotatedCorners(spec);
    const Point tl = imageToScreen.apply(corners[0]);
    const Point tr = imageToScreen.apply(corners[1]);
    const Point center = imageToScreen.apply(annotationCenter(spec));
    const Point mid{(tl.x + tr.x) * 0.5f, (tl.y + tr.y) * 0.5f};
    // 矩形なら中心→上辺中点は上辺に垂直。高さ 0(直線等)で中点が中心に一致したら真上へ
    float dx = mid.x - center.x;
    float dy = mid.y - center.y;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len < 0.001f) {
        dx = 0;
        dy = -1;
    } else {
        dx /= len;
        dy /= len;
    }
    return {mid.x + dx * offsetPx, mid.y + dy * offsetPx};
}

float angleDegFrom(Point center, Point p) {
    return std::atan2(p.y - center.y, p.x - center.x) * 180.0f / kPi;
}

float snapAngleDeg(float deg, float step) {
    return std::round(deg / step) * step;
}

float normalizeAngleDeg(float deg) {
    const float m = std::fmod(deg, 360.0f);
    return m < 0 ? m + 360.0f : m;
}

} // namespace blinker
