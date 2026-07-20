#pragma once

#include <algorithm>
#include <array>
#include <filesystem>
#include <string>
#include <string_view>

#include "core/str_util.h"
#include "core/unicode.h"

/**
 * @file image_formats.h
 * @brief 対応画像形式の拡張子一覧と判定。
 */

namespace blinker {

/**
 * @brief 対応拡張子の一覧(小文字、先頭ドット付き)。
 *
 * Windows は WIC 標準コーデック + Windows 11 の拡張コーデック (HEIF/WebP/AVIF)。
 * コーデック(デコーダ実装)がない環境では該当形式はデコード失敗として扱われる。
 */
inline constexpr std::array<std::string_view, 16> kImageExtensions = {
    ".jpg", ".jpeg", ".jpe", ".jfif",
    ".png", ".bmp",  ".dib", ".gif",
    ".tif", ".tiff", ".ico",
    ".webp", ".heic", ".heif", ".avif",
    ".jxr",
};

/**
 * @brief パスの拡張子が対応画像形式かを判定する。
 * @param[in] path 判定するパス。実在しなくてもよい(拡張子だけを見る)。
 * @return kImageExtensions に含まれる拡張子なら true。大文字小文字は区別しない。
 */
inline bool isImageFile(const std::filesystem::path& path) {
    // 拡張子は ASCII 前提のため toLower(ASCII のみ)で足りる
    const std::string ext = toLower(pathToUtf8(path.extension()));
    return std::find(kImageExtensions.begin(), kImageExtensions.end(), std::string_view(ext)) !=
           kImageExtensions.end();
}

} // namespace blinker
