#include "core/annotation_edit.h"

#include <algorithm>
#include <cmath>

namespace blinker {
namespace {

constexpr float kPi = 3.14159265358979323846f;

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

Point rotateAround(Point p, Point center, float deg) {
    const float rad = deg * kPi / 180.0f;
    const float c = std::cos(rad);
    const float s = std::sin(rad);
    const float dx = p.x - center.x;
    const float dy = p.y - center.y;
    return {center.x + dx * c - dy * s, center.y + dx * s + dy * c};
}

Point constrainToSquare(Point anchor, Point p) {
    const float dx = p.x - anchor.x;
    const float dy = p.y - anchor.y;
    const float side = std::min(std::abs(dx), std::abs(dy));
    return {anchor.x + std::copysign(side, dx), anchor.y + std::copysign(side, dy)};
}

Point constrainToAxis(Point anchor, Point p) {
    const float dx = p.x - anchor.x;
    const float dy = p.y - anchor.y;
    const float adx = std::abs(dx);
    const float ady = std::abs(dy);
    // 8 方向の境界は 22.5 度ごと。tan(22.5°) と tan(67.5°) で場合分けする
    constexpr float kTan22_5 = 0.41421356f;
    constexpr float kTan67_5 = 2.41421356f;
    if (ady <= adx * kTan22_5) return {p.x, anchor.y};  // 水平
    if (ady >= adx * kTan67_5) return {anchor.x, p.y};  // 垂直
    return constrainToSquare(anchor, p);                // 45 度
}

BoundsF annotationBounds(const AnnotationSpec& spec) {
    return {std::min(spec.p1.x, spec.p2.x), std::min(spec.p1.y, spec.p2.y),
            std::max(spec.p1.x, spec.p2.x), std::max(spec.p1.y, spec.p2.y)};
}

bool appendPenPoint(std::vector<Point>& points, Point p, float minDistance) {
    if (!points.empty() && minDistance > 0) {
        const float dx = p.x - points.back().x;
        const float dy = p.y - points.back().y;
        if (dx * dx + dy * dy < minDistance * minDistance) return false;
    }
    points.push_back(p);
    return true;
}

void updatePenBounds(AnnotationSpec& spec) {
    if (spec.points.empty()) return;
    BoundsF b{spec.points.front().x, spec.points.front().y, spec.points.front().x,
              spec.points.front().y};
    for (const Point& p : spec.points) {
        b.minX = std::min(b.minX, p.x);
        b.minY = std::min(b.minY, p.y);
        b.maxX = std::max(b.maxX, p.x);
        b.maxY = std::max(b.maxY, p.y);
    }
    spec.p1 = {b.minX, b.minY};
    spec.p2 = {b.maxX, b.maxY};
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
    // 塗りつぶしてある図形は内部も掴めるようにする(見た目どおりの当たり判定にする)
    const bool filled = spec.fillAlpha > 0;
    switch (spec.kind) {
    case AnnotationSpec::Kind::Rect: {
        const bool insideOuter = q.x >= b.minX - reach && q.x <= b.maxX + reach &&
                                 q.y >= b.minY - reach && q.y <= b.maxY + reach;
        const bool insideInner = q.x > b.minX + reach && q.x < b.maxX - reach &&
                                 q.y > b.minY + reach && q.y < b.maxY - reach;
        return insideOuter && (filled || !insideInner);
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
        return inside(rx + reach, ry + reach) && (filled || !inside(rx - reach, ry - reach));
    }
    case AnnotationSpec::Kind::Line:
    case AnnotationSpec::Kind::Arrow:
        return pointSegmentDistance(q, spec.p1, spec.p2) <= reach;
    case AnnotationSpec::Kind::Pen: {
        // 手書きは線の上だけ当たり(bbox 内部で判定すると、囲むように描いた線の
        // 内側が丸ごと掴めてしまい、下の図形を選べなくなる)
        if (spec.points.empty()) return false;
        if (spec.points.size() == 1) {
            const float dx = q.x - spec.points.front().x;
            const float dy = q.y - spec.points.front().y;
            return dx * dx + dy * dy <= reach * reach;
        }
        for (size_t i = 1; i < spec.points.size(); ++i) {
            if (pointSegmentDistance(q, spec.points[i - 1], spec.points[i]) <= reach) {
                return true;
            }
        }
        return false;
    }
    case AnnotationSpec::Kind::Number: {
        // 連番マーカーは中身の詰まった円なので内部も当たりにする
        const float rx = (b.maxX - b.minX) * 0.5f + reach;
        const float ry = (b.maxY - b.minY) * 0.5f + reach;
        if (rx <= 0 || ry <= 0) return false;
        const float nx = (q.x - c.x) / rx;
        const float ny = (q.y - c.y) / ry;
        return nx * nx + ny * ny <= 1.0f;
    }
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
    for (Point& p : spec.points) {  // 手書きの点列も一緒に動かす(bbox との同期を保つ)
        p.x += dx;
        p.y += dy;
    }
}

std::vector<ResizeHandlePos> resizeHandlePositions(const AnnotationSpec& spec) {
    if (spec.kind == AnnotationSpec::Kind::Line || spec.kind == AnnotationSpec::Kind::Arrow) {
        const Point c = annotationCenter(spec);
        return {{ResizeHandle::P1, rotateAround(spec.p1, c, spec.angleDeg)},
                {ResizeHandle::P2, rotateAround(spec.p2, c, spec.angleDeg)}};
    }
    const auto corners = rotatedCorners(spec);  // TL TR BR BL
    const auto mid = [](Point a, Point b) {
        return Point{(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f};
    };
    // 連番マーカーは円を保つため四隅だけ(辺ハンドルを出すと楕円にできてしまう)
    if (spec.kind == AnnotationSpec::Kind::Number) {
        return {{ResizeHandle::TopLeft, corners[0]},
                {ResizeHandle::TopRight, corners[1]},
                {ResizeHandle::BottomRight, corners[2]},
                {ResizeHandle::BottomLeft, corners[3]}};
    }
    std::vector<ResizeHandlePos> handles{
        {ResizeHandle::TopLeft, corners[0]},     {ResizeHandle::TopRight, corners[1]},
        {ResizeHandle::BottomRight, corners[2]}, {ResizeHandle::BottomLeft, corners[3]},
        {ResizeHandle::Left, mid(corners[3], corners[0])},
        {ResizeHandle::Right, mid(corners[1], corners[2])},
    };
    // テキストの高さは内容から決まるため上下辺のハンドルは出さない
    if (spec.kind != AnnotationSpec::Kind::Text) {
        handles.push_back({ResizeHandle::Top, mid(corners[0], corners[1])});
        handles.push_back({ResizeHandle::Bottom, mid(corners[2], corners[3])});
    }
    return handles;
}

AnnotationSpec resizeAnnotation(const AnnotationSpec& orig, ResizeHandle handle,
                                Point mouseImage, bool keepAspect) {
    AnnotationSpec spec = orig;
    const Point c0 = annotationCenter(orig);

    // 端点ドラッグ (Line/Arrow): 他端の見た目の位置を固定したまま端点を mouse へ。
    // 線分の bbox 中心 = 端点の中点で、回転で中点は不変なので新しい中心は
    // world 上の両端点の中点になる
    if (handle == ResizeHandle::P1 || handle == ResizeHandle::P2) {
        const Point otherWorld = rotateAround(
            handle == ResizeHandle::P1 ? orig.p2 : orig.p1, c0, orig.angleDeg);
        // Shift 中は固定端から見て水平・垂直・45 度へ寄せる(見た目どおりに
        // 揃えたいので、回転を戻す前の world 座標で判定する)
        const Point moved = keepAspect ? constrainToAxis(otherWorld, mouseImage) : mouseImage;
        const Point c1{(moved.x + otherWorld.x) * 0.5f, (moved.y + otherWorld.y) * 0.5f};
        const Point movedLocal = rotateAround(moved, c1, -orig.angleDeg);
        const Point otherLocal = rotateAround(otherWorld, c1, -orig.angleDeg);
        if (handle == ResizeHandle::P1) {
            spec.p1 = movedLocal;
            spec.p2 = otherLocal;
        } else {
            spec.p1 = otherLocal;
            spec.p2 = movedLocal;
        }
        return spec;
    }

    const bool left = handle == ResizeHandle::TopLeft || handle == ResizeHandle::Left ||
                      handle == ResizeHandle::BottomLeft;
    const bool right = handle == ResizeHandle::TopRight || handle == ResizeHandle::Right ||
                       handle == ResizeHandle::BottomRight;
    const bool top = handle == ResizeHandle::TopLeft || handle == ResizeHandle::Top ||
                     handle == ResizeHandle::TopRight;
    const bool bottom = handle == ResizeHandle::BottomLeft || handle == ResizeHandle::Bottom ||
                        handle == ResizeHandle::BottomRight;

    // マウスを回転前のローカル座標へ戻し、掴んだ辺だけを動かす(反対側は越えない)
    const BoundsF b = annotationBounds(orig);
    const Point m = rotateAround(mouseImage, c0, -orig.angleDeg);
    constexpr float kMinSize = 1.0f;
    BoundsF nb = b;
    if (left) nb.minX = std::min(m.x, b.maxX - kMinSize);
    if (right) nb.maxX = std::max(m.x, b.minX + kMinSize);
    if (top) nb.minY = std::min(m.y, b.maxY - kMinSize);
    if (bottom) nb.maxY = std::max(m.y, b.minY + kMinSize);

    // 連番マーカーは常に円のままにする(縦横比を崩さない)
    if ((keepAspect || orig.kind == AnnotationSpec::Kind::Number) && (left || right) &&
        (top || bottom)) {
        const float w0 = b.maxX - b.minX;
        const float h0 = b.maxY - b.minY;
        if (w0 > 0 && h0 > 0) {
            const float scale =
                std::max((nb.maxX - nb.minX) / w0, (nb.maxY - nb.minY) / h0);
            if (left) nb.minX = nb.maxX - w0 * scale;
            if (right) nb.maxX = nb.minX + w0 * scale;
            if (top) nb.minY = nb.maxY - h0 * scale;
            if (bottom) nb.maxY = nb.minY + h0 * scale;
        }
    }

    // 回転はサイズで動く bbox 中心周りのため、そのままだと反対側がずれる。
    // アンカー(掴んだ辺の反対側の角/辺中点)の world 位置を保つよう平行移動する
    const auto anchorLocal = [left, right, top, bottom](const BoundsF& r) {
        const float x = left ? r.maxX : right ? r.minX : (r.minX + r.maxX) * 0.5f;
        const float y = top ? r.maxY : bottom ? r.minY : (r.minY + r.maxY) * 0.5f;
        return Point{x, y};
    };
    const Point anchor0 = rotateAround(anchorLocal(b), c0, orig.angleDeg);
    const Point c1{(nb.minX + nb.maxX) * 0.5f, (nb.minY + nb.maxY) * 0.5f};
    const Point anchor1 = rotateAround(anchorLocal(nb), c1, orig.angleDeg);
    const float dx = anchor0.x - anchor1.x;
    const float dy = anchor0.y - anchor1.y;
    spec.p1 = {nb.minX + dx, nb.minY + dy};
    spec.p2 = {nb.maxX + dx, nb.maxY + dy};
    // 手書きは点列が実体なので、bbox の変化と同じ比率で写す(線幅は変えない)
    if (!spec.points.empty()) {
        const float sx = b.maxX > b.minX ? (spec.p2.x - spec.p1.x) / (b.maxX - b.minX) : 1.0f;
        const float sy = b.maxY > b.minY ? (spec.p2.y - spec.p1.y) / (b.maxY - b.minY) : 1.0f;
        for (Point& p : spec.points) {
            p.x = spec.p1.x + (p.x - b.minX) * sx;
            p.y = spec.p1.y + (p.y - b.minY) * sy;
        }
    }
    return spec;
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
