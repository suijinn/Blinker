#include "sdl/file_system_posix.h"

#include <algorithm>
#include <string>
#include <system_error>
#include <utility>

#include "core/str_util.h"
#include "core/unicode.h"
#include "platform/image_formats.h"

namespace blinker {
namespace {

namespace fs = std::filesystem;

// ソートキーは「親フォルダの相対パス」と「ファイル名」の 2 段(win 版と同じ理由。
// file_system_win.cpp のコメント参照)。比較回数ぶんの UTF-8 変換を避けるため
// キーは列挙時に 1 回だけ作る
struct Keyed {
    std::string parent;
    std::string name;
    FileEntry entry;
};

bool isHiddenDirectory(const fs::path& path) {
    const std::string name = pathToUtf8(path.filename());
    return !name.empty() && name.front() == '.';
}

Keyed makeKeyed(const fs::directory_entry& entry, const fs::path& dir, const bool recursive) {
    Keyed keyed;
    keyed.entry.path = entry.path();
    keyed.entry.relative =
        recursive ? entry.path().lexically_relative(dir) : entry.path().filename();
    keyed.parent = pathToUtf8(keyed.entry.relative.parent_path());
    keyed.name = pathToUtf8(keyed.entry.relative.filename());
    // 時刻は大小比較にしか使わない(FileEntry::lastWriteTick を参照)
    std::error_code ec;
    if (const auto time = entry.last_write_time(ec); !ec) {
        keyed.entry.lastWriteTick = static_cast<int64_t>(time.time_since_epoch().count());
    }
    if (const auto size = entry.file_size(ec); !ec) keyed.entry.sizeBytes = size;
    return keyed;
}

} // namespace

ListResult FileSystemPosix::listImages(const fs::path& dir, const ListOptions& options) {
    std::vector<Keyed> keyed;
    bool truncated = false;
    std::error_code ec;
    const auto full = [&keyed, &options] {
        return options.maxFiles != 0 && keyed.size() >= options.maxFiles;
    };

    if (options.recursive) {
        // follow_directory_symlink を指定しないので、シンボリックリンクは辿らない
        // (辿ると循環したツリーで無限に走査してしまう)
        fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied,
                                            ec);
        for (const fs::recursive_directory_iterator end; !ec && it != end; it.increment(ec)) {
            if (it->is_directory(ec)) {
                if (isHiddenDirectory(it->path())) it.disable_recursion_pending();
                continue;
            }
            if (!it->is_regular_file(ec) || !isImageFile(it->path())) continue;
            if (full()) {
                truncated = true;
                break;
            }
            keyed.push_back(makeKeyed(*it, dir, true));
        }
    } else {
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec) || !isImageFile(it->path())) continue;
            if (full()) {
                truncated = true;
                break;
            }
            keyed.push_back(makeKeyed(*it, dir, false));
        }
    }

    std::sort(keyed.begin(), keyed.end(), [](const Keyed& a, const Keyed& b) {
        if (const int c = naturalCompare(a.parent, b.parent); c != 0) return c < 0;
        return naturalCompare(a.name, b.name) < 0;
    });

    ListResult result;
    result.truncated = truncated;
    result.entries.reserve(keyed.size());
    for (Keyed& k : keyed) result.entries.push_back(std::move(k.entry));
    return result;
}

} // namespace blinker
