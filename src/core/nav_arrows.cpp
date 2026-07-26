#include "core/nav_arrows.h"

#include <algorithm>

namespace blinker {
namespace {

// ボタンが 2 つ収まらない狭いビューポートでは出さない(画像が見えなくなるため)
constexpr float kMinViewportW = 4 * kNavArrowMarginPx + 3 * kNavArrowSizePx;
constexpr float kMinViewportH = 2 * kNavArrowMarginPx + kNavArrowSizePx;

bool contains(const NavArrow& arrow, Point p) {
    return arrow.visible && p.x >= arrow.p1.x && p.x <= arrow.p2.x && p.y >= arrow.p1.y &&
           p.y <= arrow.p2.y;
}

} // namespace

NavArrowsState navArrowsState(SizeF viewport, std::optional<Point> pointer, bool hasPrev,
                             bool hasNext) {
    NavArrowsState state;
    if (!pointer) return state;
    if (viewport.w < kMinViewportW || viewport.h < kMinViewportH) return state;
    // ポインタがビューポートの外(サイドバー・ステータスバー上など)なら出さない
    if (pointer->x < 0 || pointer->x > viewport.w || pointer->y < 0 || pointer->y > viewport.h) {
        return state;
    }

    // 帯は端から一定距離。狭い窓で左右の帯が重ならないよう幅の 1/4 で抑える
    const float band = std::min(kNavArrowBandPx, viewport.w * 0.25f);
    const float top = (viewport.h - kNavArrowSizePx) / 2;
    const float bottom = top + kNavArrowSizePx;

    state.prev.visible = hasPrev && pointer->x <= band;
    state.prev.p1 = {kNavArrowMarginPx, top};
    state.prev.p2 = {kNavArrowMarginPx + kNavArrowSizePx, bottom};
    state.next.visible = hasNext && pointer->x >= viewport.w - band;
    state.next.p1 = {viewport.w - kNavArrowMarginPx - kNavArrowSizePx, top};
    state.next.p2 = {viewport.w - kNavArrowMarginPx, bottom};

    state.prev.hovered = contains(state.prev, *pointer);
    state.next.hovered = contains(state.next, *pointer);
    return state;
}

std::optional<bool> hitTestNavArrows(const NavArrowsState& state, Point pointer) {
    if (contains(state.next, pointer)) return true;
    if (contains(state.prev, pointer)) return false;
    return std::nullopt;
}

} // namespace blinker
