#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace blinker {

// UTF-8 ⇔ UTF-16/32 の純 C++ 変換。core/platform 層の文字列は UTF-8 の
// std::string に統一し、OS API が UTF-16 を要求する境界(win 層)でだけ変換する。
// 不正なバイト列・サロゲートは U+FFFD (REPLACEMENT CHARACTER) に置き換える(例外なし)。

// s の pos から 1 コードポイントをデコードして pos を進める。事前条件: pos < s.size()
char32_t decodeUtf8(std::string_view s, size_t& pos);

void appendUtf8(std::string& out, char32_t cp);

std::u32string utf8ToUtf32(std::string_view s);
std::string utf32ToUtf8(std::u32string_view s);

// wchar_t は Windows では UTF-16、Linux/macOS では UTF-32 として扱う
std::wstring utf8ToWide(std::string_view s);
std::string wideToUtf8(std::wstring_view s);

// std::filesystem::path とのブリッジ(ネイティブ表現 ⇔ UTF-8)
std::string pathToUtf8(const std::filesystem::path& p);
// 区切りを '/' に正規化した表現(パス同士の比較用)
std::string pathToUtf8Generic(const std::filesystem::path& p);
std::filesystem::path pathFromUtf8(std::string_view s);

} // namespace blinker
