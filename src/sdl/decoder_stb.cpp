#include "sdl/decoder_stb.h"

#include <cstdio>

#include "core/unicode.h"
#include "stb/stb_image.h"

namespace blinker {
namespace {

// RGBA(ストレート)→ BGRA(事前乗算)へ変換して DecodedImage を作る
std::shared_ptr<DecodedImage> imageFromRgba(const stbi_uc* data, int w, int h) {
    if (!data || w <= 0 || h <= 0) return nullptr;
    auto image = std::make_shared<DecodedImage>();
    image->width = static_cast<uint32_t>(w);
    image->height = static_cast<uint32_t>(h);
    image->pixels.resize(static_cast<size_t>(w) * h * 4);
    uint8_t* out = image->pixels.data();
    const size_t count = static_cast<size_t>(w) * h;
    for (size_t i = 0; i < count; ++i) {
        const stbi_uc* px = data + i * 4;
        const uint32_t a = px[3];
        out[i * 4 + 0] = static_cast<uint8_t>((px[2] * a + 127) / 255);  // B
        out[i * 4 + 1] = static_cast<uint8_t>((px[1] * a + 127) / 255);  // G
        out[i * 4 + 2] = static_cast<uint8_t>((px[0] * a + 127) / 255);  // R
        out[i * 4 + 3] = static_cast<uint8_t>(a);
    }
    return image;
}

} // namespace

std::shared_ptr<DecodedImage> DecoderStb::decode(const std::filesystem::path& path) {
    // POSIX のパスはネイティブが UTF-8 のためそのまま fopen できる
    int w = 0, h = 0, comp = 0;
    stbi_uc* data = stbi_load(pathToUtf8(path).c_str(), &w, &h, &comp, 4);
    if (!data) return nullptr;
    auto image = imageFromRgba(data, w, h);
    stbi_image_free(data);
    return image;
}

std::shared_ptr<DecodedImage> decodeFromMemory(const uint8_t* data, size_t size) {
    if (!data || size == 0) return nullptr;
    int w = 0, h = 0, comp = 0;
    stbi_uc* pixels =
        stbi_load_from_memory(data, static_cast<int>(size), &w, &h, &comp, 4);
    if (!pixels) return nullptr;
    auto image = imageFromRgba(pixels, w, h);
    stbi_image_free(pixels);
    return image;
}

} // namespace blinker
