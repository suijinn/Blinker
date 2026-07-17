#include "core/image_list.h"

#include <algorithm>
#include <cwctype>

namespace blinker {
namespace {

// Windows のパスは大文字小文字を区別しないため、無視して等価判定する
bool equalsIgnoreCase(const std::wstring& a, const std::wstring& b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](wchar_t x, wchar_t y) {
               return std::towlower(x) == std::towlower(y);
           });
}

} // namespace

void ImageList::set(std::vector<std::filesystem::path> files,
                    const std::filesystem::path& current) {
    files_ = std::move(files);
    index_ = 0;
    if (current.empty()) return;
    const std::wstring target = current.generic_wstring();
    for (size_t i = 0; i < files_.size(); ++i) {
        if (equalsIgnoreCase(files_[i].generic_wstring(), target)) {
            index_ = i;
            break;
        }
    }
}

void ImageList::clear() {
    files_.clear();
    index_ = 0;
}

bool ImageList::next() {
    if (index_ + 1 >= files_.size()) return false;
    ++index_;
    return true;
}

bool ImageList::prev() {
    if (files_.empty() || index_ == 0) return false;
    --index_;
    return true;
}

bool ImageList::first() {
    if (files_.empty() || index_ == 0) return false;
    index_ = 0;
    return true;
}

bool ImageList::last() {
    if (files_.empty() || index_ + 1 == files_.size()) return false;
    index_ = files_.size() - 1;
    return true;
}

std::vector<std::filesystem::path> ImageList::prefetchOrder(int radius) const {
    std::vector<std::filesystem::path> out;
    for (int d = 1; d <= radius; ++d) {
        const size_t distance = static_cast<size_t>(d);
        if (index_ + distance < files_.size()) out.push_back(files_[index_ + distance]);
        if (distance <= index_) out.push_back(files_[index_ - distance]);
    }
    return out;
}

} // namespace blinker
