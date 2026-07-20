#include "core/text_edit.h"

#include <algorithm>

namespace blinker {
namespace {

bool isContinuation(char c) {
    return (static_cast<unsigned char>(c) & 0xC0) == 0x80;
}

/// 語の切れ目を判定するための文字種。同じ種別の連なりが 1 語になる
enum class CharClass { Space, Word, Other };

CharClass classify(const std::string& s, size_t pos) {
    const unsigned char c = static_cast<unsigned char>(s[pos]);
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') return CharClass::Space;
    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        c == '_') {
        return CharClass::Word;
    }
    return CharClass::Other;  // 記号と非 ASCII(日本語など)。連なりをまとめて選ぶ
}

} // namespace

TextEditBuffer::TextEditBuffer(std::string text, std::vector<TextStyleRun> styles)
    : text_(std::move(text)), styles_(std::move(styles)) {
    normalizeTextStyles(styles_);
    caret_ = text_.size();
    anchor_ = caret_;
}

size_t TextEditBuffer::clampToBoundary(size_t offset) const {
    if (offset >= text_.size()) return text_.size();
    while (offset > 0 && isContinuation(text_[offset])) --offset;
    return offset;
}

std::string TextEditBuffer::selectedText() const {
    return text_.substr(selectionBegin(), selectionEnd() - selectionBegin());
}

void TextEditBuffer::setCaret(size_t offset, bool extendSelection) {
    caret_ = clampToBoundary(offset);
    if (!extendSelection) anchor_ = caret_;
}

void TextEditBuffer::insert(std::string_view utf8) {
    deleteSelection();
    text_.insert(caret_, utf8);
    adjustTextStyles(styles_, caret_, 0, utf8.size());
    caret_ += utf8.size();
    anchor_ = caret_;
}

bool TextEditBuffer::deleteSelection() {
    if (!hasSelection()) return false;
    const size_t begin = selectionBegin();
    const size_t length = selectionEnd() - begin;
    text_.erase(begin, length);
    adjustTextStyles(styles_, begin, length, 0);
    caret_ = begin;
    anchor_ = begin;
    return true;
}

bool TextEditBuffer::backspace() {
    if (deleteSelection()) return true;
    if (caret_ == 0) return false;
    size_t prev = caret_ - 1;
    while (prev > 0 && isContinuation(text_[prev])) --prev;
    text_.erase(prev, caret_ - prev);
    adjustTextStyles(styles_, prev, caret_ - prev, 0);
    caret_ = prev;
    anchor_ = prev;
    return true;
}

bool TextEditBuffer::deleteForward() {
    if (deleteSelection()) return true;
    if (caret_ >= text_.size()) return false;
    size_t next = caret_ + 1;
    while (next < text_.size() && isContinuation(text_[next])) ++next;
    text_.erase(caret_, next - caret_);
    adjustTextStyles(styles_, caret_, next - caret_, 0);
    anchor_ = caret_;
    return true;
}

bool TextEditBuffer::selectionHasFlag(TextStyleFlag flag) const {
    return isTextStyleFlagSet(styles_, selectionBegin(), selectionEnd(), flag);
}

bool TextEditBuffer::toggleSelectionFlag(TextStyleFlag flag) {
    if (!hasSelection()) return false;
    const std::vector<TextStyleRun> before = styles_;
    setTextStyleFlag(styles_, selectionBegin(), selectionEnd(), flag, !selectionHasFlag(flag));
    return styles_ != before;
}

bool TextEditBuffer::setSelectionColor(uint32_t colorRGB) {
    if (!hasSelection()) return false;
    const std::vector<TextStyleRun> before = styles_;
    setTextStyleColor(styles_, selectionBegin(), selectionEnd(), colorRGB);
    return styles_ != before;
}

TextStyleRun TextEditBuffer::selectionStyle() const {
    return textStyleAt(styles_, selectionBegin());
}

void TextEditBuffer::moveLeft(bool extendSelection) {
    if (!extendSelection && hasSelection()) {
        caret_ = selectionBegin();  // 選択解除は左端へ寄せる(一般的なエディタと同じ)
        anchor_ = caret_;
        return;
    }
    if (caret_ > 0) setCaret(caret_ - 1, extendSelection);
    else if (!extendSelection) anchor_ = caret_;
}

void TextEditBuffer::moveRight(bool extendSelection) {
    if (!extendSelection && hasSelection()) {
        caret_ = selectionEnd();
        anchor_ = caret_;
        return;
    }
    if (caret_ >= text_.size()) {
        if (!extendSelection) anchor_ = caret_;
        return;
    }
    size_t next = caret_ + 1;
    while (next < text_.size() && isContinuation(text_[next])) ++next;
    setCaret(next, extendSelection);
}

void TextEditBuffer::moveLineStart(bool extendSelection) {
    const size_t nl = text_.rfind('\n', caret_ == 0 ? 0 : caret_ - 1);
    setCaret(nl == std::string::npos || caret_ == 0 ? 0 : nl + 1, extendSelection);
}

void TextEditBuffer::moveLineEnd(bool extendSelection) {
    const size_t nl = text_.find('\n', caret_);
    setCaret(nl == std::string::npos ? text_.size() : nl, extendSelection);
}

void TextEditBuffer::selectAll() {
    anchor_ = 0;
    caret_ = text_.size();
}

void TextEditBuffer::selectWordAt(size_t offset) {
    offset = clampToBoundary(offset);
    if (text_.empty()) {
        caret_ = 0;
        anchor_ = 0;
        return;
    }
    // 末尾クリックでは直前の文字の語を選ぶ
    size_t probe = offset < text_.size() ? offset : clampToBoundary(text_.size() - 1);
    const CharClass cls = classify(text_, probe);
    size_t begin = probe;
    while (begin > 0) {
        size_t prev = begin - 1;
        while (prev > 0 && isContinuation(text_[prev])) --prev;
        if (classify(text_, prev) != cls) break;
        begin = prev;
    }
    size_t end = probe;
    while (end < text_.size() && classify(text_, end) == cls) {
        ++end;
        while (end < text_.size() && isContinuation(text_[end])) ++end;
    }
    anchor_ = begin;
    caret_ = end;
}

} // namespace blinker
