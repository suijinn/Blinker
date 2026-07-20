#pragma once

#include "platform/file_system.h"

/**
 * @file file_system_posix.h
 * @brief ファイル列挙(SDL バックエンド用)。
 */

namespace blinker {

/**
 * @brief std::filesystem によるファイル列挙。
 *
 * ソートは core の naturalCompare(エクスプローラ相当の自然順)。
 */
class FileSystemPosix final : public IFileSystem {
public:
    /**
     * @brief フォルダ直下の画像ファイルを列挙する。
     * @param[in] dir 列挙するフォルダ。
     * @return naturalCompare による自然順に並んだ画像パス。フォルダが読めない場合は空。
     */
    std::vector<std::filesystem::path> listImages(const std::filesystem::path& dir) override;
};

} // namespace blinker
