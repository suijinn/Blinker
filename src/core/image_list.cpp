#include "core/image_list.h"

#include <algorithm>
#include <cctype>
#include <string>

#include "core/unicode.h"

namespace blinker {
namespace {

// Windows のパスは大文字小文字を区別しないため、無視して等価判定する。
// UTF-8 のマルチバイト部は 0x80 以上で ASCII と衝突しないため、ASCII のみの
// 大文字小文字無視で安全に比較できる(非 ASCII はバイト一致)
bool equalsIgnoreCase(const std::string& a, const std::string& b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) ==
                      std::tolower(static_cast<unsigned char>(y));
           });
}

} // namespace

void ImageList::set(std::vector<std::filesystem::path> files,
                    const std::filesystem::path& current) {
    files_ = std::move(files);
    index_ = 0;
    if (current.empty()) return;
    const std::string target = pathToUtf8Generic(current);
    for (size_t i = 0; i < files_.size(); ++i) {
        if (equalsIgnoreCase(pathToUtf8Generic(files_[i]), target)) {
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

bool ImageList::jumpTo(size_t index) {
    if (index >= files_.size() || index == index_) return false;
    index_ = index;
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
