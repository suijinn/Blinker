#pragma once

#include "platform/file_system.h"

namespace blinker {

// std::filesystem で列挙し、エクスプローラと同じ自然順 (StrCmpLogicalW) でソートする。
class FileSystemWin final : public IFileSystem {
public:
    std::vector<std::filesystem::path> listImages(const std::filesystem::path& dir) override;
};

} // namespace blinker
