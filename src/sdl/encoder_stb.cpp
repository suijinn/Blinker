#include "sdl/encoder_stb.h"

#include <string>

#include "core/pixel_convert.h"
#include "core/str_util.h"
#include "core/unicode.h"
#include "stb/stb_image_write.h"

namespace blinker {
namespace {

// PBGRA → ストレート RGBA(PNG 用)
std::vector<uint8_t> toStraightRGBA(const DecodedImage& image) {
    std::vector<uint8_t> out(image.pixels.size());
    for (size_t i = 0; i + 3 < image.pixels.size(); i += 4) {
        const uint8_t a = image.pixels[i + 3];
        out[i + 0] = unpremultiply(image.pixels[i + 2], a);  // R
        out[i + 1] = unpremultiply(image.pixels[i + 1], a);  // G
        out[i + 2] = unpremultiply(image.pixels[i + 0], a);  // B
        out[i + 3] = a;
    }
    return out;
}

// PBGRA → 白背景に合成した 24bpp RGB(JPEG/BMP 用)
std::vector<uint8_t> toOpaqueRGB(const DecodedImage& image) {
    const size_t count = static_cast<size_t>(image.width) * image.height;
    std::vector<uint8_t> out(count * 3);
    for (size_t i = 0; i < count; ++i) {
        const uint8_t* px = image.pixels.data() + i * 4;
        const uint32_t inverse = 255u - px[3];  // 事前乗算のため白 * (1 - α) を足すだけ
        out[i * 3 + 0] = static_cast<uint8_t>(px[2] + (255u * inverse + 127) / 255);
        out[i * 3 + 1] = static_cast<uint8_t>(px[1] + (255u * inverse + 127) / 255);
        out[i * 3 + 2] = static_cast<uint8_t>(px[0] + (255u * inverse + 127) / 255);
    }
    return out;
}

void appendToVector(void* context, void* data, int size) {
    auto* out = static_cast<std::vector<uint8_t>*>(context);
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    out->insert(out->end(), bytes, bytes + size);
}

} // namespace

bool EncoderStb::encode(const DecodedImage& image, const std::filesystem::path& path) {
    if (image.width == 0 || image.height == 0) return false;
    const std::string ext = toLower(pathToUtf8(path.extension()));
    const std::string file = pathToUtf8(path);
    const int w = static_cast<int>(image.width);
    const int h = static_cast<int>(image.height);
    if (ext == ".png") {
        const std::vector<uint8_t> rgba = toStraightRGBA(image);
        return stbi_write_png(file.c_str(), w, h, 4, rgba.data(), w * 4) != 0;
    }
    if (ext == ".jpg" || ext == ".jpeg") {
        const std::vector<uint8_t> rgb = toOpaqueRGB(image);
        return stbi_write_jpg(file.c_str(), w, h, 3, rgb.data(), 90) != 0;
    }
    if (ext == ".bmp") {
        const std::vector<uint8_t> rgb = toOpaqueRGB(image);
        return stbi_write_bmp(file.c_str(), w, h, 3, rgb.data()) != 0;
    }
    return false;
}

std::vector<uint8_t> encodePngToMemory(const DecodedImage& image) {
    if (image.width == 0 || image.height == 0) return {};
    const std::vector<uint8_t> rgba = toStraightRGBA(image);
    std::vector<uint8_t> out;
    if (!stbi_write_png_to_func(appendToVector, &out, static_cast<int>(image.width),
                                static_cast<int>(image.height), 4, rgba.data(),
                                static_cast<int>(image.width) * 4)) {
        return {};
    }
    return out;
}

} // namespace blinker
