#include "core/exif.h"

#include <cstring>
#include <new>
#include <utility>

namespace blinker {
namespace {

constexpr uint16_t kOrientationTag = 0x0112;  // TIFF/Exif の Orientation
constexpr uint16_t kTypeShort = 3;            // 同タグの型 (SHORT)

uint16_t read16(const uint8_t* p, bool bigEndian) {
    return bigEndian ? static_cast<uint16_t>(p[0] << 8 | p[1])
                     : static_cast<uint16_t>(p[1] << 8 | p[0]);
}

uint32_t read32(const uint8_t* p, bool bigEndian) {
    const uint32_t a = p[0], b = p[1], c = p[2], d = p[3];
    return bigEndian ? (a << 24 | b << 16 | c << 8 | d) : (d << 24 | c << 16 | b << 8 | a);
}

// TIFF ヘッダ(Exif 本体)の先頭から IFD0 の Orientation を探す。
// Exif の SubIFD は見ない(Orientation は IFD0 に置かれる)
uint16_t orientationFromTiff(const uint8_t* tiff, size_t size) {
    if (size < 8) return 1;
    bool bigEndian = false;
    if (tiff[0] == 'I' && tiff[1] == 'I') {
        bigEndian = false;
    } else if (tiff[0] == 'M' && tiff[1] == 'M') {
        bigEndian = true;
    } else {
        return 1;
    }
    if (read16(tiff + 2, bigEndian) != 0x002A) return 1;
    const size_t ifd = read32(tiff + 4, bigEndian);
    if (ifd < 8 || ifd + 2 > size) return 1;
    const uint16_t entryCount = read16(tiff + ifd, bigEndian);
    for (uint16_t i = 0; i < entryCount; ++i) {
        const size_t entry = ifd + 2 + static_cast<size_t>(i) * 12;  // エントリは 12 バイト固定
        if (entry + 12 > size) break;
        if (read16(tiff + entry, bigEndian) != kOrientationTag) continue;
        if (read16(tiff + entry + 2, bigEndian) != kTypeShort) break;
        // 値が 4 バイト以内なら、値へのオフセットではなく値そのものが入っている
        const uint16_t value = read16(tiff + entry + 8, bigEndian);
        return (value >= 1 && value <= 8) ? value : 1;
    }
    return 1;
}

// JPEG のセグメントを辿って APP1 の Exif を探す
uint16_t orientationFromJpeg(const uint8_t* data, size_t size) {
    size_t pos = 2;  // SOI (FFD8) の後ろから
    while (pos + 4 <= size) {
        if (data[pos] != 0xFF) return 1;  // マーカー境界を失ったら諦める
        const uint8_t marker = data[pos + 1];
        if (marker == 0xFF) {  // 詰め物の FF は読み飛ばす
            ++pos;
            continue;
        }
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD9)) {
            pos += 2;  // 長さフィールドを持たないマーカー
            continue;
        }
        if (marker == 0xDA) return 1;  // 画像データの開始。これより後ろに Exif は無い
        const size_t length = read16(data + pos + 2, true);
        if (length < 2 || pos + 2 + length > size) return 1;
        const uint8_t* payload = data + pos + 4;
        const size_t payloadSize = length - 2;
        if (marker == 0xE1 && payloadSize > 6 && std::memcmp(payload, "Exif\0\0", 6) == 0) {
            return orientationFromTiff(payload + 6, payloadSize - 6);
        }
        pos += 2 + length;
    }
    return 1;
}

// PNG のチャンクを辿って eXIf を探す(IDAT の後ろに置かれていることもある)
uint16_t orientationFromPng(const uint8_t* data, size_t size) {
    size_t pos = 8;  // 署名の後ろから
    while (pos + 12 <= size) {
        const uint32_t length = read32(data + pos, true);
        const uint8_t* type = data + pos + 4;
        if (length > size || pos + 12 + length > size) return 1;  // 末尾 4 バイトは CRC
        if (std::memcmp(type, "eXIf", 4) == 0) return orientationFromTiff(data + pos + 8, length);
        if (std::memcmp(type, "IEND", 4) == 0) return 1;
        pos += 12 + length;
    }
    return 1;
}

} // namespace

uint16_t readExifOrientation(const uint8_t* data, size_t size) {
    if (!data || size < 12) return 1;
    if (data[0] == 0xFF && data[1] == 0xD8) return orientationFromJpeg(data, size);
    constexpr uint8_t kPngSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (std::memcmp(data, kPngSignature, sizeof(kPngSignature)) == 0) {
        return orientationFromPng(data, size);
    }
    return 1;  // 他の形式(BMP/GIF/TGA 等)は Orientation を持たない
}

bool applyExifOrientation(DecodedImage& image, uint16_t orientation) {
    if (orientation <= 1 || orientation > 8) return false;
    const size_t w = image.width;
    const size_t h = image.height;
    if (w == 0 || h == 0 || image.pixels.size() < w * h * 4) return false;

    // 5〜8 は転置を伴うため出力の縦横が入れ替わる
    const bool transposed = orientation >= 5;
    const size_t dstW = transposed ? h : w;
    const size_t dstH = transposed ? w : h;

    std::vector<uint8_t> out;
    try {
        out.resize(w * h * 4);
    } catch (const std::bad_alloc&) {
        return false;
    }

    const uint8_t* src = image.pixels.data();
    uint8_t* dst = out.data();
    for (size_t y = 0; y < dstH; ++y) {
        for (size_t x = 0; x < dstW; ++x) {
            // 出力画素 (x, y) の取得元 (sx, sy) を求める
            size_t sx = 0;
            size_t sy = 0;
            switch (orientation) {
            case 2: sx = w - 1 - x; sy = y;             break;  // 左右反転
            case 3: sx = w - 1 - x; sy = h - 1 - y;     break;  // 180 度
            case 4: sx = x;         sy = h - 1 - y;     break;  // 上下反転
            case 5: sx = y;         sy = x;             break;  // 主対角で転置
            case 6: sx = y;         sy = h - 1 - x;     break;  // 時計回り 90 度
            case 7: sx = w - 1 - y; sy = h - 1 - x;     break;  // 副対角で転置
            case 8: sx = w - 1 - y; sy = x;             break;  // 時計回り 270 度
            default: break;
            }
            const uint8_t* s = src + (sy * w + sx) * 4;
            uint8_t* d = dst + (y * dstW + x) * 4;
            d[0] = s[0];
            d[1] = s[1];
            d[2] = s[2];
            d[3] = s[3];
        }
    }

    image.pixels = std::move(out);
    image.width = static_cast<uint32_t>(dstW);
    image.height = static_cast<uint32_t>(dstH);
    return true;
}

} // namespace blinker
