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
     * @brief フォルダ直下の画像ファイルを列挙する。
     * @param[in] dir 列挙するフォルダ。
     * @return StrCmpLogicalW による自然順に並んだ画像パス。フォルダが読めない場合は空。
     */
    std::vector<std::filesystem::path> listImages(const std::filesystem::path& dir) override;
};

} // namespace blinker
