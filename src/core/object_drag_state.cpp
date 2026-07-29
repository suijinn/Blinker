#include "core/object_drag_state.h"

namespace blinker {

void ObjectDragState::beginMove(Point imagePos, const AnnotationSpec& spec) {
    mode_ = ObjectDragMode::Move;
    startImage_ = imagePos;
    origSpec_ = spec;
}

void ObjectDragState::beginRotate(const AnnotationSpec& spec, float pointerAngleDeg) {
    mode_ = ObjectDragMode::Rotate;
    origSpec_ = spec;
    startAngleDeg_ = pointerAngleDeg;
}

void ObjectDragState::beginResize(const AnnotationSpec& spec, ResizeHandle handle) {
    mode_ = ObjectDragMode::Resize;
    origSpec_ = spec;
    handle_ = handle;
}

}  // namespace blinker
