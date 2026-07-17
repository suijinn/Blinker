#include "core/dib.h"

#include <bit>

namespace blinker {
namespace {

// windows.h の BI_* に相当(core は OS ヘッダを include しないため自前定義)
constexpr uint32_t kCompressionRGB = 0;        // BI_RGB
constexpr uint32_t kCompressionBitfields = 3;  // BI_BITFIELDS

// DecoderWic と同じ上限(D2D の ID2D1Bitmap が確実に扱える範囲)
constexpr uint32_t kMaxDimension = 16384;

uint16_t readU16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint32_t readU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

int32_t readI32(const uint8_t* p) {
    return static_cast<int32_t>(readU32(p));
}

// ビットマスク1チャンネル分の抽出。0..255 へのスケーリングを事前計算する
struct ChannelMask {
    uint32_t mask = 0;
    int shift = 0;
    uint32_t maxValue = 0;  // マスクが表せる最大値。0 ならチャンネルなし

    void set(uint32_t m) {
        mask = m;
        shift = m ? std::countr_zero(m) : 0;
        maxValue = m >> shift;
    }

    uint8_t extract(uint32_t pixel) const {
        if (maxValue == 0) return 0;
        const uint32_t v = (pixel & mask) >> shift;
        if (maxValue == 255) return static_cast<uint8_t>(v);
        // 5bit (555/565) 等を 0..255 へスケール(四捨五入)
        return static_cast<uint8_t>((v * 255u + maxValue / 2) / maxValue);
    }
};

} // namespace

std::shared_ptr<DecodedImage> imageFromDib(const uint8_t* data, size_t size) {
    if (!data || size < 40) return nullptr;
    const uint32_t headerSize = readU32(data);
    if (headerSize < 40 || headerSize > size) return nullptr;

    const int32_t rawWidth = readI32(data + 4);
    const int32_t rawHeight = readI32(data + 8);  // 負 = トップダウン
    const uint16_t bitCount = readU16(data + 14);
    const uint32_t compression = readU32(data + 16);
    const uint32_t clrUsed = readU32(data + 32);

    if (rawWidth <= 0 || rawHeight == 0) return nullptr;
    const bool topDown = rawHeight < 0;
    const uint64_t height64 = topDown ? -static_cast<int64_t>(rawHeight) : rawHeight;
    const uint32_t width = static_cast<uint32_t>(rawWidth);
    if (width > kMaxDimension || height64 > kMaxDimension) return nullptr;
    const uint32_t height = static_cast<uint32_t>(height64);

    if (bitCount != 8 && bitCount != 16 && bitCount != 24 && bitCount != 32) return nullptr;
    if (compression != kCompressionRGB && compression != kCompressionBitfields) return nullptr;
    if (compression == kCompressionBitfields && bitCount != 16 && bitCount != 32) return nullptr;

    ChannelMask r, g, b, a;
    size_t maskBytes = 0;  // 40バイトヘッダの直後に置かれるマスク領域のサイズ
    if (compression == kCompressionBitfields) {
        if (headerSize >= 52) {
            // BITMAPV2 以降 (V4/V5 含む): マスクはヘッダ内
            r.set(readU32(data + 40));
            g.set(readU32(data + 44));
            b.set(readU32(data + 48));
            if (headerSize >= 56) a.set(readU32(data + 52));
        } else {
            // BITMAPINFOHEADER: RGB の 3 DWORD マスクがヘッダ直後に続く
            if (size < 40 + 12) return nullptr;
            r.set(readU32(data + 40));
            g.set(readU32(data + 44));
            b.set(readU32(data + 48));
            maskBytes = 12;
        }
        if (r.mask == 0 || g.mask == 0 || b.mask == 0) return nullptr;
    } else if (bitCount == 16) {
        r.set(0x7C00);  // BI_RGB の 16bpp は 555 固定
        g.set(0x03E0);
        b.set(0x001F);
    } else if (bitCount >= 24) {
        r.set(0x00FF0000);
        g.set(0x0000FF00);
        b.set(0x000000FF);
        // 32bpp BI_RGB の第4バイトは規格上未定義 → 原則として不透明扱い。
        // ただし V4/V5 ヘッダが明示的にアルファマスクを持つ場合はそれに従う
        if (bitCount == 32 && headerSize >= 108) a.set(readU32(data + 52));
    }

    size_t paletteEntries = clrUsed;
    if (bitCount == 8 && paletteEntries == 0) paletteEntries = 256;
    if (paletteEntries > 256) return nullptr;
    const size_t paletteOffset = headerSize + maskBytes;
    const size_t pixelOffset = paletteOffset + paletteEntries * 4;
    if (pixelOffset > size) return nullptr;
    const uint8_t* palette = data + paletteOffset;

    const size_t stride = ((static_cast<size_t>(width) * bitCount + 31) / 32) * 4;
    if (size - pixelOffset < stride * height) return nullptr;

    auto image = std::make_shared<DecodedImage>();
    image->width = width;
    image->height = height;
    image->pixels.resize(static_cast<size_t>(width) * height * 4);

    const bool hasAlpha = a.mask != 0;
    bool anyAlpha = false;
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* src = data + pixelOffset + stride * (topDown ? y : height - 1 - y);
        uint8_t* dst = image->pixels.data() + static_cast<size_t>(y) * width * 4;
        for (uint32_t x = 0; x < width; ++x) {
            uint8_t* out = dst + static_cast<size_t>(x) * 4;
            switch (bitCount) {
            case 8: {
                const uint8_t index = src[x];
                if (index < paletteEntries) {
                    const uint8_t* entry = palette + static_cast<size_t>(index) * 4;  // BGRX
                    out[0] = entry[0];
                    out[1] = entry[1];
                    out[2] = entry[2];
                } else {
                    out[0] = out[1] = out[2] = 0;
                }
                out[3] = 255;
                break;
            }
            case 16: {
                const uint32_t px = readU16(src + static_cast<size_t>(x) * 2);
                out[0] = b.extract(px);
                out[1] = g.extract(px);
                out[2] = r.extract(px);
                out[3] = hasAlpha ? a.extract(px) : 255;
                break;
            }
            case 24: {
                const uint8_t* p = src + static_cast<size_t>(x) * 3;
                out[0] = p[0];
                out[1] = p[1];
                out[2] = p[2];
                out[3] = 255;
                break;
            }
            case 32: {
                const uint32_t px = readU32(src + static_cast<size_t>(x) * 4);
                out[0] = b.extract(px);
                out[1] = g.extract(px);
                out[2] = r.extract(px);
                out[3] = hasAlpha ? a.extract(px) : 255;
                break;
            }
            }
            anyAlpha = anyAlpha || out[3] != 0;
        }
    }

    if (hasAlpha && !anyAlpha) {
        // アルファマスク付きなのに全ピクセル a=0 の DIB を作るアプリがある
        // (実質は不透明画像)。そのまま乗算すると全面透明になるため不透明として扱う
        for (size_t i = 3; i < image->pixels.size(); i += 4) image->pixels[i] = 255;
    } else if (hasAlpha) {
        // ストレートアルファ → 事前乗算 (四捨五入)
        for (size_t i = 0; i < image->pixels.size(); i += 4) {
            const uint8_t alpha = image->pixels[i + 3];
            if (alpha == 255) continue;
            for (int c = 0; c < 3; ++c) {
                image->pixels[i + c] =
                    static_cast<uint8_t>((image->pixels[i + c] * alpha + 127) / 255);
            }
        }
    }
    return image;
}

} // namespace blinker
