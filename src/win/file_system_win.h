#pragma once

#include "platform/file_system.h"

/**
 * @file file_system_win.h
 * @brief ファイル列挙(Windows 実装)。
 */

namespace blinker {

/**
 * @brief std::filesystem で列挙し、エクスプローラと同じ自然順 (StrCmpLogicalW) でソートする。
 */
class FileSystemWin final : public IFileSystem {
public:
    /**
     * @brief フォルダ内の画像ファイルを列挙する。
     * @param[in] dir     列挙の起点フォルダ。
     * @param[in] options 再帰の有無と件数上限。
     * @return StrCmpLogicalW による「親フォルダ → ファイル名」の自然順に並んだ結果。
     *         フォルダが読めない場合は空。
     * @note 再帰時はシンボリックリンク(ジャンクション)と隠しフォルダを辿らない。
     */
    ListResult listImages(const std::filesystem::path& dir, const ListOptions& options) override;
};

} // namespace blinker
