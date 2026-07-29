#include "core/image_origin.h"

#include <utility>

namespace blinker {

void ImageOrigin::setFile(std::filesystem::path path) {
    path_ = std::move(path);
    error_.clear();
    clipboard_ = false;
    failed_ = false;
}

void ImageOrigin::setFailed(std::filesystem::path path, std::string error) {
    path_ = std::move(path);
    error_ = std::move(error);
    clipboard_ = false;
    failed_ = true;
}

void ImageOrigin::setLoading() {
    // パスは直前に表示していたものを残す(前の画像を出したまま待つため)
    error_.clear();
    clipboard_ = false;
    failed_ = false;
}

void ImageOrigin::setClipboard() {
    path_.clear();  // 一覧に戻ったとき必ず再取得させる
    error_.clear();
    clipboard_ = true;
    failed_ = false;
}

void ImageOrigin::clear() {
    path_.clear();
    error_.clear();
    clipboard_ = false;
    failed_ = false;
}

bool ImageOrigin::setEdited(const bool edited) {
    if (edited_ == edited) return false;
    edited_ = edited;
    return true;
}

}  // namespace blinker
