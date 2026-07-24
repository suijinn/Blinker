#include "sdl/clipboard_sdl.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <system_error>
#include <vector>

#include "sdl/decoder_stb.h"
#include "sdl/encoder_stb.h"

namespace blinker {
namespace {

constexpr const char* kMimePng = "image/png";
constexpr const char* kMimeUriList = "text/uri-list";
constexpr const char* kMimeGnome = "x-special/gnome-copied-files";

// SDL_SetClipboardData は要求時コールバックでデータを渡す方式のため、
// エンコード済み PNG をクリップボードの生存期間だけ保持する
struct PngHolder {
    std::vector<uint8_t> bytes;
};

const void* pngDataCallback(void* userdata, const char* mimeType, size_t* size) {
    auto* holder = static_cast<PngHolder*>(userdata);
    if (!holder || !mimeType || std::strcmp(mimeType, kMimePng) != 0) {
        if (size) *size = 0;
        return nullptr;
    }
    if (size) *size = holder->bytes.size();
    return holder->bytes.data();
}

void pngCleanupCallback(void* userdata) {
    delete static_cast<PngHolder*>(userdata);
}

// パスを file:// URI にする(RFC 3986 の unreserved と '/' 以外をパーセントエンコード)
std::string toFileUri(const std::filesystem::path& path) {
    std::error_code ec;
    const std::filesystem::path full = std::filesystem::absolute(path, ec);
    const std::string utf8 = (ec ? path : full).generic_string();

    std::string uri = "file://";
    for (const char c : utf8) {
        const auto byte = static_cast<unsigned char>(c);
        const bool unreserved = (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
                                (byte >= '0' && byte <= '9') || byte == '-' || byte == '.' ||
                                byte == '_' || byte == '~' || byte == '/';
        if (unreserved) {
            uri += c;
        } else {
            uri += std::format("%{:02X}", byte);
        }
    }
    return uri;
}

// uri-list と gnome 形式の両方を、クリップボードの生存期間だけ保持する
struct FilesHolder {
    std::string uriList;  ///< text/uri-list(CRLF 区切り)
    std::string gnome;    ///< x-special/gnome-copied-files("copy" + LF + URI)
};

const void* filesDataCallback(void* userdata, const char* mimeType, size_t* size) {
    auto* holder = static_cast<FilesHolder*>(userdata);
    const std::string* data = nullptr;
    if (holder && mimeType) {
        if (std::strcmp(mimeType, kMimeUriList) == 0) {
            data = &holder->uriList;
        } else if (std::strcmp(mimeType, kMimeGnome) == 0) {
            data = &holder->gnome;
        }
    }
    if (size) *size = data ? data->size() : 0;
    return data ? data->data() : nullptr;
}

void filesCleanupCallback(void* userdata) {
    delete static_cast<FilesHolder*>(userdata);
}

} // namespace

bool ClipboardSdl::setImage(const DecodedImage& image) {
    auto holder = new PngHolder{encodePngToMemory(image)};
    if (holder->bytes.empty()) {
        delete holder;
        return false;
    }
    const char* mimeTypes[] = {kMimePng};
    if (!SDL_SetClipboardData(pngDataCallback, pngCleanupCallback, holder, mimeTypes, 1)) {
        delete holder;
        return false;
    }
    return true;  // holder は cleanup コールバックで解放される
}

bool ClipboardSdl::setFiles(const std::vector<std::filesystem::path>& paths) {
    if (paths.empty()) return false;

    auto holder = new FilesHolder{};
    holder->gnome = "copy";
    for (const auto& path : paths) {
        const std::string uri = toFileUri(path);
        holder->uriList += uri + "\r\n";  // text/uri-list の行区切りは CRLF
        holder->gnome += "\n" + uri;
    }

    const char* mimeTypes[] = {kMimeUriList, kMimeGnome};
    if (!SDL_SetClipboardData(filesDataCallback, filesCleanupCallback, holder, mimeTypes, 2)) {
        delete holder;
        return false;
    }
    return true;  // holder は cleanup コールバックで解放される
}

bool ClipboardSdl::setText(const std::string& text) {
    return SDL_SetClipboardText(text.c_str());
}

std::string ClipboardSdl::getText() {
    char* text = SDL_GetClipboardText();  // 失敗時も空文字列を返す(nullptr にはならない)
    if (!text) return {};
    std::string out(text);
    SDL_free(text);
    out.erase(std::remove(out.begin(), out.end(), '\r'), out.end());  // 改行を LF に統一
    return out;
}

std::shared_ptr<DecodedImage> ClipboardSdl::getImage() {
    size_t size = 0;
    void* data = SDL_GetClipboardData(kMimePng, &size);
    if (!data) return nullptr;
    auto image = decodeFromMemory(static_cast<const uint8_t*>(data), size);
    SDL_free(data);
    return image;
}

} // namespace blinker
