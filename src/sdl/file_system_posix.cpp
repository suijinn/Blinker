#include "sdl/file_system_posix.h"

#include <algorithm>
#include <string>
#include <system_error>

#include "core/str_util.h"
#include "core/unicode.h"
#include "platform/image_formats.h"

namespace blinker {

std::vector<std::filesystem::path> FileSystemPosix::listImages(
    const std::filesystem::path& dir) {
    namespace fs = std::filesystem;
    std::vector<fs::path> files;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        if (isImageFile(it->path())) files.push_back(it->path());
    }
    // ファイル名の UTF-8 表現をキーに自然順ソート(比較回数分の変換を避ける)
    std::vector<std::pair<std::string, fs::path>> keyed;
    keyed.reserve(files.size());
    for (auto& f : files) keyed.emplace_back(pathToUtf8(f.filename()), std::move(f));
    std::sort(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) {
        return naturalCompare(a.first, b.first) < 0;
    });
    std::vector<fs::path> out;
    out.reserve(keyed.size());
    for (auto& [key, path] : keyed) out.push_back(std::move(path));
    return out;
}

} // namespace blinker
