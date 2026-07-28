#include "win/file_system_win.h"

#include <windows.h>

#include <shlwapi.h>

#include <algorithm>
#include <string>
#include <utility>

#include "platform/image_formats.h"

namespace blinker {
namespace {

namespace fs = std::filesystem;

// ソートキーは「親フォルダの相対パス」と「ファイル名」の 2 段。相対パスを 1 本の
// 文字列として比較すると、区切りの '\' (0x5C) が英数字より後ろに来るため
// `a\b.jpg` と `a.jpg` の並びが直感に反する
struct Keyed {
    std::wstring parent;
    std::wstring name;
    FileEntry entry;
};

// .git のような大量のファイルを抱えるフォルダを再帰で踏まないための判定
bool isHiddenDirectory(const fs::path& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_HIDDEN) != 0;
}

Keyed makeKeyed(const fs::directory_entry& entry, const fs::path& dir, const bool recursive) {
    Keyed keyed;
    keyed.entry.path = entry.path();
    keyed.entry.relative =
        recursive ? entry.path().lexically_relative(dir) : entry.path().filename();
    keyed.parent = keyed.entry.relative.parent_path().native();
    keyed.name = keyed.entry.relative.filename().native();
    // directory_entry は列挙時の属性をキャッシュしているので追加の I/O は起きない。
    // 時刻は大小比較にしか使わない(FileEntry::lastWriteTick を参照)
    std::error_code ec;
    if (const auto time = entry.last_write_time(ec); !ec) {
        keyed.entry.lastWriteTick = static_cast<int64_t>(time.time_since_epoch().count());
    }
    if (const auto size = entry.file_size(ec); !ec) keyed.entry.sizeBytes = size;
    return keyed;
}

} // namespace

ListResult FileSystemWin::listImages(const fs::path& dir, const ListOptions& options) {
    std::vector<Keyed> keyed;
    bool truncated = false;
    std::error_code ec;
    const auto full = [&keyed, &options] {
        return options.maxFiles != 0 && keyed.size() >= options.maxFiles;
    };

    if (options.recursive) {
        // follow_directory_symlink を指定しないので、シンボリックリンクとジャンクションは
        // 辿らない(辿ると循環したツリーで無限に走査してしまう)
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
        for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec),
             end;
             !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec) || !isImageFile(it->path())) continue;
            if (full()) {
                truncated = true;
                break;
            }
            keyed.push_back(makeKeyed(*it, dir, false));
        }
    }

    std::sort(keyed.begin(), keyed.end(), [](const Keyed& a, const Keyed& b) {
        if (const int c = StrCmpLogicalW(a.parent.c_str(), b.parent.c_str()); c != 0) return c < 0;
        return StrCmpLogicalW(a.name.c_str(), b.name.c_str()) < 0;
    });

    ListResult result;
    result.truncated = truncated;
    result.entries.reserve(keyed.size());
    for (Keyed& k : keyed) result.entries.push_back(std::move(k.entry));
    return result;
}

} // namespace blinker
