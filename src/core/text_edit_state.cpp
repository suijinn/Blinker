#include "core/text_edit_state.h"

#include <algorithm>
#include <utility>

namespace blinker {

void TextEditState::begin(size_t index, bool created, std::string text,
                          std::vector<TextStyleRun> styles) {
    active_ = true;
    index_ = index;
    created_ = created;
    mouseSelecting_ = false;
    caretOn_ = true;
    styleMenuPending_ = false;
    resetComposition();
    // キャレットは末尾。部分書式も引き継いで、続きの入力が直前の書式を継ぐようにする
    buffer_ = TextEditBuffer(std::move(text), std::move(styles));
}

void TextEditState::end() {
    active_ = false;
    mouseSelecting_ = false;
    resetComposition();  // 変換中なら捨てる(host 側も IME へキャンセルを通知する)
}

bool TextEditState::consumeStyleMenu() {
    return std::exchange(styleMenuPending_, false);
}

void TextEditState::setComposition(const std::string& utf8, size_t caretBytes,
                                   size_t targetBegin, size_t targetEnd) {
    composition_ = utf8;
    compositionCaret_ = std::min(caretBytes, composition_.size());
    compositionTargetBegin_ = std::min(targetBegin, composition_.size());
    compositionTargetEnd_ = std::clamp(targetEnd, compositionTargetBegin_, composition_.size());
}

void TextEditState::resetComposition() {
    composition_.clear();
    compositionCaret_ = 0;
    compositionTargetBegin_ = 0;
    compositionTargetEnd_ = 0;
}

std::string TextEditState::displayText() const {
    if (composition_.empty()) return buffer_.text();
    std::string out = buffer_.text();
    out.insert(buffer_.caret(), composition_);
    return out;
}

std::vector<TextStyleRun> TextEditState::displayStyles() const {
    std::vector<TextStyleRun> styles = buffer_.styles();
    // 変換中文字列を挿入した分だけ後ろの書式をずらす(挿入と同じ扱い)
    if (!composition_.empty()) {
        adjustTextStyles(styles, buffer_.caret(), 0, composition_.size());
    }
    return styles;
}

size_t TextEditState::caretOffset() const {
    return buffer_.caret() + (composition_.empty() ? 0 : compositionCaret_);
}

} // namespace blinker
