#include "sdl/decoder_stb.h"

#include <cstdlib>
#include <format>
#include <fstream>
#include <limits>
#include <system_error>
#include <vector>

#include "core/exif.h"
#include "core/str_util.h"
#include "core/unicode.h"
#include "stb/stb_image.h"

namespace blinker {
namespace {

// ファイル全体をメモリへ読む。EXIF を読むために先頭のバイト列が必要で、
// 同じバッファをそのままデコードにも使う(ファイルを二度開かない)
std::vector<uint8_t> readAllBytes(const std::filesystem::path& path) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size == 0 || size > std::numeric_limits<int>::max()) return {};
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    bytes.resize(static_cast<size_t>(in.gcount()));  // 途中まで読めた分で続ける
    return bytes;
}

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

std::shared_ptr<DecodedImage> DecoderStb::decode(const std::filesystem::path& path,
                                                 std::string* error) {
    const std::vector<uint8_t> bytes = readAllBytes(path);
    if (bytes.empty()) {
        if (error) *error = "ファイル読み込み失敗";
        return nullptr;
    }
    int w = 0, h = 0, comp = 0;
    stbi_uc* data =
        stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &comp, 4);
    if (!data) {
        if (error) {
            const char* reason = stbi_failure_reason();
            *error = std::format("デコード失敗 ({})", reason ? reason : "原因不明");
        }
        return nullptr;
    }
    auto image = imageFromRgba(data, w, h);
    stbi_image_free(data);
    if (!image) {
        if (error) *error = std::format("PBGRA変換 ({} x {})", w, h);
        return nullptr;
    }
    // stb_image は EXIF を読まないので、向きは自前で解析して適用する
    applyExifOrientation(*image, readExifOrientation(bytes.data(), bytes.size()));
    return image;
}

SequenceInfo DecoderStb::probeSequence(const std::filesystem::path& path) {
    SequenceInfo info;
    // stb_image にはフレーム数だけを安く数える API が無いので、拡張子で当たりを付ける。
    // 実際に 1 フレームしか無ければ decodeAnimation が false を返して静止画のまま残る
    if (toLower(pathToUtf8(path.extension())) != ".gif") return info;
    info.kind = SequenceKind::Animation;
    info.frameCount = 2;  // 「2 以上かもしれない」という意味しか持たない(decoder.h 参照)
    info.loopCount = 0;   // stb は NETSCAPE 拡張を返さないので無限ループ扱い
    return info;
}

bool DecoderStb::decodeAnimation(const std::filesystem::path& path,
                                 const AnimationLimits& limits, ImageSequence& out,
                                 std::string* error) {
    const std::vector<uint8_t> bytes = readAllBytes(path);
    if (bytes.empty()) {
        if (error) *error = "ファイル読み込み失敗";
        return false;
    }
    int* delays = nullptr;
    int w = 0, h = 0, frameCount = 0, comp = 0;
    // 全フレームが 1 つの確保にまとめて返る(Disposal は stb 側で処理済み)
    stbi_uc* data = stbi_load_gif_from_memory(bytes.data(), static_cast<int>(bytes.size()),
                                              &delays, &w, &h, &frameCount, &comp, 4);
    if (!data) {
        if (error) {
            const char* reason = stbi_failure_reason();
            *error = std::format("GIF展開失敗 ({})", reason ? reason : "原因不明");
        }
        return false;
    }
    struct Guard {  // 早期 return が多いので確実に解放する
        stbi_uc* pixels;
        int* delays;
        ~Guard() {
            stbi_image_free(pixels);
            // 遅延の配列は stb の既定アロケータ (malloc) で確保される。
            // STBI_FREE は実装 TU (stb_impl.cpp) の中だけのマクロなのでここでは使えない
            std::free(delays);
        }
    } guard{data, delays};

    if (w <= 0 || h <= 0 || frameCount < 2) return false;  // 静止画のまま扱う
    const size_t totalBytes = static_cast<size_t>(w) * h * 4 * frameCount;
    if (static_cast<uint32_t>(frameCount) > limits.maxFrames || totalBytes > limits.maxBytes) {
        if (error) *error = std::format("展開が上限を超える ({} フレーム)", frameCount);
        out.truncated = true;
        return false;
    }

    out.kind = SequenceKind::Animation;
    out.loopCount = 0;  // 無限ループ(stb はループ回数を返さない)
    out.frames.clear();
    out.frames.reserve(static_cast<size_t>(frameCount));
    const size_t frameStride = static_cast<size_t>(w) * h * 4;
    for (int i = 0; i < frameCount; ++i) {
        auto image = imageFromRgba(data + frameStride * static_cast<size_t>(i), w, h);
        if (!image) {
            if (error) *error = std::format("PBGRA変換 ({} x {})", w, h);
            return false;
        }
        // stb の遅延はミリ秒。0 のフレームは App 側で既定値へ読み替えられる
        out.frames.push_back(
            FrameEntry{std::move(image), delays ? static_cast<uint32_t>(delays[i]) : 0, {}});
    }
    return true;
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
