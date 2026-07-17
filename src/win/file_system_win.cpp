#include "win/file_system_win.h"

#include <windows.h>

#include <shlwapi.h>

#include <algorithm>

#include "platform/image_formats.h"

namespace blinker {

std::vector<std::filesystem::path> FileSystemWin::listImages(const std::filesystem::path& dir) {
    namespace fs = std::filesystem;
    std::vector<fs::path> files;
    std::error_code ec;
    for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec) && isImageFile(it->path())) {
            files.push_back(it->path());
        }
    }
    std::sort(files.begin(), files.end(), [](const fs::path& a, const fs::path& b) {
        return StrCmpLogicalW(a.filename().c_str(), b.filename().c_str()) < 0;
    });
    return files;
}

} // namespace blinker
