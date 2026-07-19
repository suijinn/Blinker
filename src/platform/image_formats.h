#pragma once

#include <algorithm>
#include <array>
#include <filesystem>
#include <string>
#include <string_view>

#include "core/str_util.h"
#include "core/unicode.h"

namespace blinker {

// 対応拡張子。Windows は WIC 標準コーデック+Windows 11 の拡張コーデック
// (HEIF/WebP/AVIF)。コーデック(デコーダ実装)がない環境では該当形式は
// デコード失敗として扱われる。
inline constexpr std::array<std::string_view, 16> kImageExtensions = {
    ".jpg", ".jpeg", ".jpe", ".jfif",
    ".png", ".bmp",  ".dib", ".gif",
    ".tif", ".tiff", ".ico",
    ".webp", ".heic", ".heif", ".avif",
    ".jxr",
};

inline bool isImageFile(const std::filesystem::path& path) {
    // 拡張子は ASCII 前提のため toLower(ASCII のみ)で足りる
    const std::string ext = toLower(pathToUtf8(path.extension()));
    return std::find(kImageExtensions.begin(), kImageExtensions.end(), std::string_view(ext)) !=
           kImageExtensions.end();
}

} // namespace blinker
