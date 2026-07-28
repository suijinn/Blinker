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
     * @brief フォルダ内の画像ファイルを列挙する。
     * @param[in] dir     列挙の起点フォルダ。
     * @param[in] options 再帰の有無と件数上限。
     * @return naturalCompare による「親フォルダ → ファイル名」の自然順に並んだ結果。
     *         フォルダが読めない場合は空。
     * @note 再帰時はシンボリックリンクと隠しフォルダ(先頭が `.`)を辿らない。
     */
    ListResult listImages(const std::filesystem::path& dir, const ListOptions& options) override;
};

} // namespace blinker
