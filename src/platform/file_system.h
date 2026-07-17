#pragma once

#include <filesystem>
#include <vector>

namespace blinker {

// ファイル列挙のプラットフォーム抽象。Windows 実装は file_system_win。
class IFileSystem {
public:
    virtual ~IFileSystem() = default;

    // dir 直下の画像ファイルを表示順(エクスプローラ相当の自然順)で返す。
    virtual std::vector<std::filesystem::path> listImages(const std::filesystem::path& dir) = 0;
};

} // namespace blinker
