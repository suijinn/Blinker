#pragma once

#include <filesystem>
#include <vector>

/**
 * @file file_system.h
 * @brief ファイル列挙のプラットフォーム抽象。
 */

namespace blinker {

/**
 * @brief ファイル列挙のプラットフォーム抽象。
 *
 * Windows 実装は file_system_win、SDL バックエンドは file_system_posix。
 */
class IFileSystem {
public:
    virtual ~IFileSystem() = default;

    /**
     * @brief フォルダ直下の画像ファイルを列挙する。
     * @param[in] dir 列挙するフォルダ。
     * @return 表示順(エクスプローラ相当の自然順)に並んだ画像パス。
     *         フォルダが読めない場合は空。
     */
    virtual std::vector<std::filesystem::path> listImages(const std::filesystem::path& dir) = 0;
};

} // namespace blinker
