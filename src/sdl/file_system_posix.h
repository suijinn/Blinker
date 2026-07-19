#pragma once

#include "platform/file_system.h"

namespace blinker {

// std::filesystem によるファイル列挙(SDL バックエンド用)。
// ソートは core の naturalCompare(エクスプローラ相当の自然順)。
class FileSystemPosix final : public IFileSystem {
public:
    std::vector<std::filesystem::path> listImages(const std::filesystem::path& dir) override;
};

} // namespace blinker
