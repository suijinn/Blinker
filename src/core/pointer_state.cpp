#include "core/pointer_state.h"

#include <cstdlib>

#include "core/mousemap.h"

namespace blinker {

MouseRole PointerState::role(MouseButton button) const {
    // 既定は左がパン・右が編集。swap_buttons でこの 2 つの役割だけを入れ替える
    const bool left = button == MouseButton::Left;
    return left != swapButtons_ ? MouseRole::Pan : MouseRole::Edit;
}

Point PointerState::moveTo(Point screenPos) {
    const Point delta{screenPos.x - lastScreen_.x, screenPos.y - lastScreen_.y};
    lastScreen_ = screenPos;
    inside_ = true;  // オーバーレイ矢印の表示判定(setInside(false) で戻す)
    return delta;
}

void PointerState::pressMenu(Point screenPos, bool onSidebar) {
    menuPressed_ = true;
    menuOnSidebar_ = onSidebar;
    menuPressScreen_ = screenPos;
}

void PointerState::cancelMenu() {
    menuPressed_ = false;
    menuOnSidebar_ = false;
}

MenuOnRelease PointerState::releaseMenu(Point screenPos) {
    const bool pressed = menuPressed_;
    const bool onSidebar = menuOnSidebar_;
    menuPressed_ = false;
    menuOnSidebar_ = false;
    if (!pressed) return MenuOnRelease::None;
    // 押した場所から動いていればドラッグだったとみなす(パン・編集ドラッグの終わり)
    const float dx = screenPos.x - menuPressScreen_.x;
    const float dy = screenPos.y - menuPressScreen_.y;
    if (dx * dx + dy * dy >= kDragThresholdPx * kDragThresholdPx) return MenuOnRelease::None;
    return onSidebar ? MenuOnRelease::Sidebar : MenuOnRelease::Pointer;
}

int PointerState::wheelSteps(float notches, bool horizontal) {
    // 1 段に達した分だけ数える。向きはコマンド側で決まっているので段数の絶対値だけを使う
    // (逆向きに回すと貯金は捨てられる)。水平だけ 1 段の重みを設定で変えられる
    float& accum = horizontal ? wheelAccumH_ : wheelAccumV_;
    const float threshold = horizontal ? horizontalThreshold_ : 1.0f;
    return std::abs(consumeWheelSteps(accum, notches, threshold));
}

}  // namespace blinker
