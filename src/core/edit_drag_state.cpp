#include "core/edit_drag_state.h"

#include "core/annotation_edit.h"

namespace blinker {

void EditDragState::begin(Point screenPos, Point imagePos, bool pen) {
    dragging_ = true;
    startScreen_ = screenPos;
    startImage_ = imagePos;
    endImage_ = imagePos;
    penPoints_.clear();
    straightAnchor_.reset();
    if (pen) penPoints_.push_back(imagePos);
}

void EditDragState::reset() {
    dragging_ = false;
    penPoints_.clear();
    straightAnchor_.reset();
}

bool EditDragState::movedEnough(Point screenPos, float thresholdPx) const {
    const float dx = screenPos.x - startScreen_.x;
    const float dy = screenPos.y - startScreen_.y;
    return dx * dx + dy * dy >= thresholdPx * thresholdPx;
}

void EditDragState::anchorStraight() {
    // 押し始めた位置を覚える。押している間は動かさないので、そこから先だけが直線になる
    if (!straightAnchor_ && !penPoints_.empty()) straightAnchor_ = penPoints_.size() - 1;
}

std::optional<Point> EditDragState::straightAnchorPoint() const {
    if (!straightAnchor_ || *straightAnchor_ >= penPoints_.size()) return std::nullopt;
    return penPoints_[*straightAnchor_];
}

void EditDragState::extendPen(float minDistancePx) {
    if (straightAnchor_ && *straightAnchor_ < penPoints_.size()) {
        // Shift 中はアンカーから先を捨てて引き直す(1 本の直線として追従させる)
        penPoints_.resize(*straightAnchor_ + 1);
        penPoints_.push_back(endImage_);
        return;
    }
    appendPenPoint(penPoints_, endImage_, minDistancePx);
}

}  // namespace blinker
