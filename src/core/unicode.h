#pragma once

#include <filesystem>
#include <string>
#include <string_view>

/**
 * @file unicode.h
 * @brief UTF-8 ⇔ UTF-16/32 の純 C++ 変換。
 *
 * core/platform 層の文字列は UTF-8 の std::string に統一し、OS API が UTF-16 を
 * 要求する境界(win 層)でだけ変換する。不正なバイト列・サロゲートは
 * U+FFFD (REPLACEMENT CHARACTER) に置き換える(例外は投げない)。
 */

namespace blinker {

/**
 * @brief s の pos から 1 コードポイントをデコードする。
 * @param[in]     s   デコード対象の UTF-8 文字列。
 * @param[in,out] pos 開始バイト位置。呼び出し後は次のコードポイントの先頭へ進む。
 * @return デコードされたコードポイント。不正なバイト列なら U+FFFD。
 * @pre pos < s.size()
 */
char32_t decodeUtf8(std::string_view s, size_t& pos);

/**
 * @brief コードポイントを UTF-8 として末尾に追記する。
 * @param[out] out 追記先の文字列。既存の内容は保持される。
 * @param[in]  cp  追記するコードポイント。不正値は U+FFFD として書き出す。
 */
void appendUtf8(std::string& out, char32_t cp);

/**
 * @brief UTF-8 を UTF-32 へ変換する。
 * @param[in] s 変換元の UTF-8 文字列。
 * @return 変換された UTF-32 文字列。
 */
std::u32string utf8ToUtf32(std::string_view s);

/**
 * @brief UTF-32 を UTF-8 へ変換する。
 * @param[in] s 変換元の UTF-32 文字列。
 * @return 変換された UTF-8 文字列。
 */
std::string utf32ToUtf8(std::u32string_view s);

/**
 * @brief UTF-8 を wchar_t 文字列へ変換する。
 * @param[in] s 変換元の UTF-8 文字列。
 * @return 変換された文字列。wchar_t は Windows では UTF-16、Linux/macOS では UTF-32。
 */
std::wstring utf8ToWide(std::string_view s);

/**
 * @brief wchar_t 文字列を UTF-8 へ変換する。
 * @param[in] s 変換元の文字列。Windows では UTF-16、Linux/macOS では UTF-32 として扱う。
 * @return 変換された UTF-8 文字列。
 */
std::string wideToUtf8(std::wstring_view s);

/**
 * @brief パスをネイティブ表現から UTF-8 へ変換する。
 * @param[in] p 変換元のパス。
 * @return UTF-8 表現。区切り文字は OS のネイティブのまま。
 */
std::string pathToUtf8(const std::filesystem::path& p);

/**
 * @brief パスを区切り '/' に正規化した UTF-8 表現へ変換する(パス同士の比較用)。
 * @param[in] p 変換元のパス。
 * @return 区切りを '/' に統一した UTF-8 表現。
 */
std::string pathToUtf8Generic(const std::filesystem::path& p);

/**
 * @brief UTF-8 文字列からパスを作る。
 * @param[in] s 変換元の UTF-8 文字列。
 * @return ネイティブ表現のパス。
 */
std::filesystem::path pathFromUtf8(std::string_view s);

} // namespace blinker
