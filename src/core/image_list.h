#pragma once

#include <filesystem>
#include <vector>

namespace blinker {

// フォルダ内の画像パス一覧と現在位置。列挙・ソートは IFileSystem 側の責務。
class ImageList {
public:
    // current が一覧に見つからない(または空パスの)場合は先頭を指す
    void set(std::vector<std::filesystem::path> files, const std::filesystem::path& current);
    void clear();

    bool empty() const { return files_.empty(); }
    size_t size() const { return files_.size(); }
    size_t index() const { return index_; }
    const std::filesystem::path& current() const { return files_[index_]; }  // 事前条件: !empty()
    const std::filesystem::path& at(size_t i) const { return files_[i]; }    // 事前条件: i < size()

    // 位置が変わったら true
    bool next();
    bool prev();
    bool first();
    bool last();
    bool jumpTo(size_t index);  // 範囲外は無視して false

    // 現在位置の近傍を優先度順に並べた先読み候補 (+1, -1, +2, -2, ...)
    std::vector<std::filesystem::path> prefetchOrder(int radius) const;

private:
    std::vector<std::filesystem::path> files_;
    size_t index_ = 0;
};

} // namespace blinker
