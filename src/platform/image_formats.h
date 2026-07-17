#pragma once

#include <algorithm>
#include <array>
#include <cwctype>
#include <filesystem>
#include <string>
#include <string_view>

namespace blinker {

// 対応拡張子。WIC 標準コーデック+Windows 11 の拡張コーデック(HEIF/WebP/AVIF)。
// 拡張コーデック未導入の環境では該当形式はデコード失敗として扱われる。
inline constexpr std::array<std::wstring_view, 16> kImageExtensions = {
    L".jpg", L".jpeg", L".jpe", L".jfif",
    L".png", L".bmp",  L".dib", L".gif",
    L".tif", L".tiff", L".ico",
    L".webp", L".heic", L".heif", L".avif",
    L".jxr",
};

inline bool isImageFile(const std::filesystem::path& path) {
    std::wstring ext = path.extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    const std::wstring_view needle = ext;
    return std::find(kImageExtensions.begin(), kImageExtensions.end(), needle) !=
           kImageExtensions.end();
}

} // namespace blinker
