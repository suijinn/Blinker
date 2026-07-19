#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS  // fopen(Windows で SDL 版を動作確認する場合)
#endif

#include "sdl/font_stb.h"

#include <cstdio>
#include <cstring>

#include "core/unicode.h"

namespace blinker {
namespace {

// 日本語グリフを持つフォントを優先した既定候補。環境により存在するものを使う
constexpr const char* kFontCandidates[] = {
    // Linux (Noto CJK / 日本語ディストリ標準)
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/opentype/noto/NotoSansCJKjp-Regular.otf",
    "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/takao-gothic/TakaoPGothic.ttf",
    "/usr/share/fonts/opentype/ipaexfont-gothic/ipaexg.ttf",
    "/usr/share/fonts/opentype/ipafont-gothic/ipag.ttf",
    // WSL2 (Windows 側のフォントを流用できる)
    "/mnt/c/Windows/Fonts/YuGothM.ttc",
    "/mnt/c/Windows/Fonts/meiryo.ttc",
    "/mnt/c/Windows/Fonts/msgothic.ttc",
    // Windows (SDL バックエンドを Windows でビルドして動作確認する場合)
    "C:/Windows/Fonts/YuGothM.ttc",
    "C:/Windows/Fonts/meiryo.ttc",
    "C:/Windows/Fonts/msgothic.ttc",
    // macOS
    "/System/Library/Fonts/ヒラギノ角ゴシック W3.ttc",
    "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
    "/System/Library/Fonts/Helvetica.ttc",
    // CJK なしのフォールバック(ASCII だけでも描けた方がよい)
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
};

uint64_t cacheKey(char32_t cp, float heightPx) {
    // 高さは 1/4px 単位で量子化してキーにする(UI 用途では十分)
    const uint64_t h = static_cast<uint64_t>(heightPx * 4.0f + 0.5f);
    return (h << 32) | static_cast<uint64_t>(cp);
}

} // namespace

bool FontStb::loadFile(const std::string& path) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) return false;
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (size <= 0) {
        std::fclose(file);
        return false;
    }
    std::vector<uint8_t> data(static_cast<size_t>(size));
    const size_t read = std::fread(data.data(), 1, data.size(), file);
    std::fclose(file);
    if (read != data.size()) return false;

    const int offset = stbtt_GetFontOffsetForIndex(data.data(), 0);  // TTC は先頭フォント
    if (offset < 0) return false;
    stbtt_fontinfo info{};
    if (!stbtt_InitFont(&info, data.data(), offset)) return false;

    fontData_ = std::move(data);
    info_ = info;
    info_.data = fontData_.data();  // move 後のポインタへ差し替え
    stbtt_GetFontVMetrics(&info_, &rawAscent_, &rawDescent_, &rawLineGap_);
    loaded_ = true;
    loadedPath_ = path;
    cache_.clear();
    return true;
}

bool FontStb::load(const std::string& configPath) {
    if (!configPath.empty() && loadFile(configPath)) return true;
    for (const char* candidate : kFontCandidates) {
        if (loadFile(candidate)) return true;
    }
    return false;
}

float FontStb::ascent(float heightPx) const {
    if (!loaded_) return heightPx;
    const float scale = stbtt_ScaleForPixelHeight(&info_, heightPx);
    return static_cast<float>(rawAscent_) * scale;
}

float FontStb::lineHeight(float heightPx) const {
    if (!loaded_) return heightPx;
    const float scale = stbtt_ScaleForPixelHeight(&info_, heightPx);
    return static_cast<float>(rawAscent_ - rawDescent_ + rawLineGap_) * scale;
}

const FontStb::Glyph& FontStb::glyphFor(char32_t cp, float heightPx) {
    const uint64_t key = cacheKey(cp, heightPx);
    if (const auto it = cache_.find(key); it != cache_.end()) return it->second;

    Glyph glyph;
    const float scale = stbtt_ScaleForPixelHeight(&info_, heightPx);
    int glyphIndex = stbtt_FindGlyphIndex(&info_, static_cast<int>(cp));
    if (glyphIndex == 0 && cp != U' ') {
        glyphIndex = stbtt_FindGlyphIndex(&info_, '?');  // 未収録グリフの代替
    }
    int advance = 0, lsb = 0;
    stbtt_GetGlyphHMetrics(&info_, glyphIndex, &advance, &lsb);
    glyph.advance = static_cast<float>(advance) * scale;

    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    stbtt_GetGlyphBitmapBox(&info_, glyphIndex, scale, scale, &x0, &y0, &x1, &y1);
    glyph.width = x1 - x0;
    glyph.height = y1 - y0;
    glyph.xoff = x0;
    glyph.yoff = y0;
    if (glyph.width > 0 && glyph.height > 0) {
        glyph.bitmap.resize(static_cast<size_t>(glyph.width) * glyph.height);
        stbtt_MakeGlyphBitmap(&info_, glyph.bitmap.data(), glyph.width, glyph.height,
                              glyph.width, scale, scale, glyphIndex);
    }
    return cache_.emplace(key, std::move(glyph)).first->second;
}

float FontStb::measure(std::string_view utf8, float heightPx) {
    if (!loaded_) return 0;
    float x = 0;
    for (size_t pos = 0; pos < utf8.size();) {
        const char32_t cp = decodeUtf8(utf8, pos);
        if (cp == U'\n') break;
        x += glyphFor(cp, heightPx).advance;
    }
    return x;
}

void FontStb::drawText(uint8_t* dst, int dstWidth, int dstHeight, int strideBytes, float x,
                       float top, std::string_view utf8, float heightPx, uint32_t rgb) {
    if (!loaded_) return;
    const float baseline = top + ascent(heightPx);
    const uint8_t r = static_cast<uint8_t>((rgb >> 16) & 0xFF);
    const uint8_t g = static_cast<uint8_t>((rgb >> 8) & 0xFF);
    const uint8_t b = static_cast<uint8_t>(rgb & 0xFF);
    float penX = x;
    for (size_t pos = 0; pos < utf8.size();) {
        const char32_t cp = decodeUtf8(utf8, pos);
        if (cp == U'\n') break;
        const Glyph& glyph = glyphFor(cp, heightPx);
        const int originX = static_cast<int>(penX + 0.5f) + glyph.xoff;
        const int originY = static_cast<int>(baseline + 0.5f) + glyph.yoff;
        for (int gy = 0; gy < glyph.height; ++gy) {
            const int py = originY + gy;
            if (py < 0 || py >= dstHeight) continue;
            for (int gx = 0; gx < glyph.width; ++gx) {
                const int px = originX + gx;
                if (px < 0 || px >= dstWidth) continue;
                const uint32_t coverage = glyph.bitmap[static_cast<size_t>(gy) * glyph.width + gx];
                if (coverage == 0) continue;
                uint8_t* out = dst + static_cast<size_t>(py) * strideBytes + static_cast<size_t>(px) * 4;
                // 事前乗算合成: out = src * cov + out * (1 - cov)
                const uint32_t inv = 255 - coverage;
                out[0] = static_cast<uint8_t>((b * coverage + out[0] * inv + 127) / 255);
                out[1] = static_cast<uint8_t>((g * coverage + out[1] * inv + 127) / 255);
                out[2] = static_cast<uint8_t>((r * coverage + out[2] * inv + 127) / 255);
                out[3] = static_cast<uint8_t>((255 * coverage + out[3] * inv + 127) / 255);
            }
        }
        penX += glyph.advance;
    }
}

} // namespace blinker
