#include "core/sidebar_state.h"

#include <algorithm>
#include <cmath>

namespace blinker {

float SidebarState::width() const {
    // 操作一覧は「操作名 + キー」が入りきらないと読めないので、狭い設定でも広げる
    return mode_ == SidebarMode::Help ? std::max(width_, kHelpWidth) : width_;
}

float SidebarState::minWidth() const {
    // 下限は width() が返す幅に揃える(操作一覧はそれ以上狭くしても見た目が
    // 変わらないので、狭められたように見えてファイル名一覧だけが縮むのを防ぐ)
    return mode_ == SidebarMode::Help ? kHelpWidth : kMinWidth;
}

bool SidebarState::toggle(SidebarMode mode) {
    // 同じモードで開いている間だけ閉じる。別のモードなら開いたまま切り替える
    enabled_ = !showing(mode);
    if (enabled_) mode_ = mode;
    return enabled_;
}

void SidebarState::setConfiguredWidth(float width) {
    width_ = std::clamp(width, kMinWidth, kMaxWidth);
}

bool SidebarState::setWidth(float width, float clientWidth) {
    const float lower = minWidth();
    const float upper = std::max(lower, std::min(kMaxWidth, clientWidth - kMinViewportWidth));
    const float clamped = std::clamp(width, lower, upper);
    if (clamped == width_) return false;
    width_ = clamped;
    return true;
}

void SidebarState::clampScroll(size_t itemCount, float viewHeight) {
    const float maxScroll =
        std::max(0.0f, static_cast<float>(itemCount) * kItemHeight - viewHeight);
    scroll_ = std::clamp(scroll_, 0.0f, maxScroll);
}

void SidebarState::scrollToItem(size_t index, size_t itemCount, float viewHeight) {
    const float itemTop = static_cast<float>(index) * kItemHeight;
    if (itemTop < scroll_) {
        scroll_ = itemTop;
    } else if (itemTop + kItemHeight > scroll_ + viewHeight) {
        scroll_ = itemTop + kItemHeight - viewHeight;
    }
    clampScroll(itemCount, viewHeight);
}

size_t SidebarState::itemAt(float y) const {
    return static_cast<size_t>((y + scroll_) / kItemHeight);
}

size_t SidebarState::firstVisibleItem() const {
    return static_cast<size_t>(scroll_ / kItemHeight);
}

float SidebarState::firstItemY() const {
    // 先頭が部分的に隠れている分だけ負になる
    return static_cast<float>(firstVisibleItem()) * kItemHeight - scroll_;
}

void SidebarState::beginResize(float screenX, float startWidth) {
    resizing_ = true;
    resizeStartX_ = screenX;
    resizeStartWidth_ = startWidth;
}

float SidebarState::resizeWidth(float screenX) const {
    return resizeStartWidth_ + screenX - resizeStartX_;
}

bool SidebarState::onResizeEdge(float screenX) const {
    return std::abs(screenX - width()) <= kResizeGripPx;
}

}  // namespace blinker
