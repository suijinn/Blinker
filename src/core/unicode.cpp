#include "core/unicode.h"

namespace blinker {

namespace {

constexpr char32_t kReplacement = 0xFFFD;

bool isContinuation(unsigned char c) {
    return (c & 0xC0) == 0x80;
}

} // namespace

char32_t decodeUtf8(std::string_view s, size_t& pos) {
    const unsigned char lead = static_cast<unsigned char>(s[pos]);
    ++pos;
    if (lead < 0x80) return lead;

    int continuations;
    char32_t cp;
    char32_t minValue;  // 冗長表現(overlong)の検出用
    if ((lead & 0xE0) == 0xC0) {
        continuations = 1;
        cp = lead & 0x1F;
        minValue = 0x80;
    } else if ((lead & 0xF0) == 0xE0) {
        continuations = 2;
        cp = lead & 0x0F;
        minValue = 0x800;
    } else if ((lead & 0xF8) == 0xF0) {
        continuations = 3;
        cp = lead & 0x07;
        minValue = 0x10000;
    } else {
        return kReplacement;  // 継続バイト単独・不正な先頭バイト
    }
    for (int i = 0; i < continuations; ++i) {
        if (pos >= s.size() || !isContinuation(static_cast<unsigned char>(s[pos]))) {
            return kReplacement;  // 途切れた列。不正バイトは次回の先頭として読み直す
        }
        cp = (cp << 6) | (static_cast<unsigned char>(s[pos]) & 0x3F);
        ++pos;
    }
    if (cp < minValue || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return kReplacement;
    return cp;
}

void appendUtf8(std::string& out, char32_t cp) {
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = kReplacement;
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

std::u32string utf8ToUtf32(std::string_view s) {
    std::u32string out;
    out.reserve(s.size());
    for (size_t pos = 0; pos < s.size();) out.push_back(decodeUtf8(s, pos));
    return out;
}

std::string utf32ToUtf8(std::u32string_view s) {
    std::string out;
    out.reserve(s.size() * 3);
    for (const char32_t cp : s) appendUtf8(out, cp);
    return out;
}

std::wstring utf8ToWide(std::string_view s) {
    std::wstring out;
    out.reserve(s.size());
    for (size_t pos = 0; pos < s.size();) {
        const char32_t cp = decodeUtf8(s, pos);
        if constexpr (sizeof(wchar_t) == 2) {
            if (cp < 0x10000) {
                out.push_back(static_cast<wchar_t>(cp));
            } else {
                const char32_t v = cp - 0x10000;
                out.push_back(static_cast<wchar_t>(0xD800 | (v >> 10)));
                out.push_back(static_cast<wchar_t>(0xDC00 | (v & 0x3FF)));
            }
        } else {
            out.push_back(static_cast<wchar_t>(cp));
        }
    }
    return out;
}

std::string wideToUtf8(std::wstring_view s) {
    std::string out;
    out.reserve(s.size() * 3);
    for (size_t i = 0; i < s.size(); ++i) {
        char32_t cp = static_cast<char32_t>(s[i]);
        if constexpr (sizeof(wchar_t) == 2) {
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                if (i + 1 < s.size()) {
                    const char32_t low = static_cast<char32_t>(s[i + 1]);
                    if (low >= 0xDC00 && low <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        ++i;
                    } else {
                        cp = kReplacement;  // 対にならないサロゲート
                    }
                } else {
                    cp = kReplacement;
                }
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                cp = kReplacement;
            }
        }
        appendUtf8(out, cp);
    }
    return out;
}

std::string pathToUtf8(const std::filesystem::path& p) {
    const std::u8string u8 = p.u8string();
    return std::string(u8.begin(), u8.end());
}

std::string pathToUtf8Generic(const std::filesystem::path& p) {
    const std::u8string u8 = p.generic_u8string();
    return std::string(u8.begin(), u8.end());
}

std::filesystem::path pathFromUtf8(std::string_view s) {
    return std::filesystem::path(
        std::u8string_view(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

} // namespace blinker
