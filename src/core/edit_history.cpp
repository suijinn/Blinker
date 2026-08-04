#include "core/edit_history.h"

#include <utility>

namespace blinker {

void EditHistory::push(EditSnapshot state) {
    pushTo(undo_, std::move(state));
    // 新しい編集をした時点で、やり直せる先(分岐した未来)は無くなる
    redo_.clear();
}

std::optional<EditSnapshot> EditHistory::undo(EditSnapshot current) {
    return transfer(undo_, redo_, std::move(current));
}

std::optional<EditSnapshot> EditHistory::redo(EditSnapshot current) {
    return transfer(redo_, undo_, std::move(current));
}

void EditHistory::clear() {
    undo_.clear();
    redo_.clear();
    resetDrag();
    resetKeyMove();
    textEditPushed_ = false;
    textEditBefore_ = {};  // 控えたままの画像を抱え込まないよう手放す
}

void EditHistory::resetDrag() {
    dragPushed_ = false;
}

bool EditHistory::consumeDragPush() {
    if (dragPushed_) return false;
    dragPushed_ = true;
    return true;
}

void EditHistory::resetKeyMove() {
    keyMovePushed_ = false;
}

bool EditHistory::consumeKeyMovePush() {
    if (keyMovePushed_) return false;
    keyMovePushed_ = true;
    return true;
}

void EditHistory::beginTextEdit(EditSnapshot before) {
    textEditPushed_ = false;
    textEditBefore_ = std::move(before);
}

std::optional<EditSnapshot> EditHistory::consumeTextEditSnapshot() {
    if (textEditPushed_) return std::nullopt;
    textEditPushed_ = true;
    return std::move(textEditBefore_);
}

void EditHistory::pushTo(std::vector<EditSnapshot>& stack, EditSnapshot state) {
    stack.push_back(std::move(state));
    if (stack.size() > kLimit) stack.erase(stack.begin());
}

std::optional<EditSnapshot> EditHistory::transfer(std::vector<EditSnapshot>& from,
                                                  std::vector<EditSnapshot>& to,
                                                  EditSnapshot current) {
    if (from.empty()) return std::nullopt;
    pushTo(to, std::move(current));  // 戻る前の状態を反対側へ積む
    EditSnapshot state = std::move(from.back());
    from.pop_back();
    return state;
}

}  // namespace blinker
