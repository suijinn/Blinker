#pragma once

#include <filesystem>
#include <vector>

/**
 * @file image_list.h
 * @brief フォルダ内の画像パス一覧と現在位置。
 */

namespace blinker {

/**
 * @brief フォルダ内の画像パス一覧と現在位置。
 *
 * 列挙・ソートは IFileSystem 側の責務で、本クラスは順序を保持するだけ。
 */
class ImageList {
public:
    /**
     * @brief 一覧と現在位置を設定する。
     * @param[in] files   表示順に並んだ画像パス。所有権を受け取る。
     * @param[in] current 現在位置にしたいパス。一覧に見つからない(または空パスの)場合は先頭を指す。
     */
    void set(std::vector<std::filesystem::path> files, const std::filesystem::path& current);

    /// @brief 一覧を空にし、現在位置を先頭へ戻す。
    void clear();

    /**
     * @brief 一覧が空かを返す。
     * @return 空なら true。
     */
    bool empty() const { return files_.empty(); }

    /**
     * @brief 一覧の要素数を返す。
     * @return 画像パスの個数。
     */
    size_t size() const { return files_.size(); }

    /**
     * @brief 現在位置のインデックスを返す。
     * @return 0 起点のインデックス。
     */
    size_t index() const { return index_; }

    /**
     * @brief 現在位置の画像パスを返す。
     * @return 現在位置のパス。
     * @pre !empty()
     */
    const std::filesystem::path& current() const { return files_[index_]; }

    /**
     * @brief 指定インデックスの画像パスを返す。
     * @param[in] i 取得するインデックス。
     * @return 該当位置のパス。
     * @pre i < size()
     */
    const std::filesystem::path& at(size_t i) const { return files_[i]; }

    /**
     * @brief 次の画像へ移動する。
     * @return 位置が変わったら true。末尾で呼ぶと false。
     */
    bool next();

    /**
     * @brief 前の画像へ移動する。
     * @return 位置が変わったら true。先頭で呼ぶと false。
     */
    bool prev();

    /**
     * @brief 先頭へ移動する。
     * @return 位置が変わったら true。
     */
    bool first();

    /**
     * @brief 末尾へ移動する。
     * @return 位置が変わったら true。
     */
    bool last();

    /**
     * @brief 指定インデックスへ移動する。
     * @param[in] index 移動先のインデックス。範囲外は無視する。
     * @return 位置が変わったら true。範囲外・同一位置なら false。
     */
    bool jumpTo(size_t index);

    /**
     * @brief 現在位置の近傍を優先度順に並べた先読み候補を返す。
     * @param[in] radius 前後何枚まで候補に含めるか。
     * @return (+1, -1, +2, -2, ...) の順に並んだパス。一覧の範囲外は含まない。
     */
    std::vector<std::filesystem::path> prefetchOrder(int radius) const;

private:
    std::vector<std::filesystem::path> files_;
    size_t index_ = 0;
};

} // namespace blinker
