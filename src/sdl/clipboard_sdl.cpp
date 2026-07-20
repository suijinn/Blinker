#include "sdl/clipboard_sdl.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "sdl/decoder_stb.h"
#include "sdl/encoder_stb.h"

namespace blinker {
namespace {

constexpr const char* kMimePng = "image/png";

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
