#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace blinker {

inline std::string toLower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

inline std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

// エクスプローラ風の自然順比較(StrCmpLogicalW 相当のポータブル実装)。
// 数字の連続は数値として比較し(数値が同じなら先頭ゼロが少ない方を先に)、
// それ以外は ASCII の大文字小文字を無視してバイト値で比較する
// (UTF-8 のマルチバイト部は 0x80 以上のためコードポイント順になる)。
// 戻り値: a < b なら負、a > b なら正、等しければ 0
inline int naturalCompare(std::string_view a, std::string_view b) {
    const auto isDigit = [](char c) { return c >= '0' && c <= '9'; };
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (isDigit(a[i]) && isDigit(b[j])) {
            const size_t si = i, sj = j;
            while (i < a.size() && isDigit(a[i])) ++i;
            while (j < b.size() && isDigit(b[j])) ++j;
            std::string_view na = a.substr(si, i - si);
            std::string_view nb = b.substr(sj, j - sj);
            const size_t za = std::min(na.find_first_not_of('0'), na.size());
            const size_t zb = std::min(nb.find_first_not_of('0'), nb.size());
            const std::string_view da = na.substr(za);  // 先頭ゼロを除いた数値部
            const std::string_view db = nb.substr(zb);
            if (da.size() != db.size()) return da.size() < db.size() ? -1 : 1;
            if (const int c = da.compare(db); c != 0) return c < 0 ? -1 : 1;
            if (za != zb) return za < zb ? -1 : 1;
            continue;
        }
        const int ca = std::tolower(static_cast<unsigned char>(a[i]));
        const int cb = std::tolower(static_cast<unsigned char>(b[j]));
        if (ca != cb) return ca < cb ? -1 : 1;
        ++i;
        ++j;
    }
    if (i < a.size()) return 1;
    if (j < b.size()) return -1;
    return 0;
}

} // namespace blinker
