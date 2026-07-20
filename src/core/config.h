#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>

/**
 * @file config.h
 * @brief blinker.ini の読み込み。
 */

namespace blinker {

/**
 * @brief blinker.ini の読み込み。
 *
 * `[section]` / `key = value` / `#` `;` コメントだけの最小 ini。
 * セクション名・キー名は小文字に正規化して保持する。
 */
class Config {
public:
    /// @brief 1 セクション分のキー → 値の対応表。
    using Section = std::unordered_map<std::string, std::string>;

    /**
     * @brief ini ファイルを読み込む。
     * @param[in] path 読み込む ini のパス。
     * @return 解析結果。ファイルが存在しない・読めない場合は空の Config。
     */
    static Config loadFile(const std::filesystem::path& path);

    /**
     * @brief ini 形式のテキストを解析する。
     * @param[in] text 解析対象のテキスト(UTF-8)。
     * @return 解析結果。
     */
    static Config parse(std::string_view text);

    /**
     * @brief セクションを取得する。
     * @param[in] name セクション名(大文字小文字は区別しない)。
     * @return 該当セクション。存在しなければ空のセクションへの参照。
     */
    const Section& section(std::string_view name) const;

    /**
     * @brief 文字列の設定値を取得する。
     * @param[in] section      セクション名。
     * @param[in] key          キー名。
     * @param[in] defaultValue 見つからない場合に返す値。
     * @return 設定値。見つからなければ defaultValue。
     */
    std::string get(std::string_view section, std::string_view key,
                    std::string defaultValue = {}) const;

    /**
     * @brief 真偽値の設定値を取得する。
     * @param[in] section      セクション名。
     * @param[in] key          キー名。
     * @param[in] defaultValue 見つからない・解釈できない場合に返す値。
     * @return 設定値。
     */
    bool getBool(std::string_view section, std::string_view key, bool defaultValue) const;

    /**
     * @brief 整数の設定値を取得する。
     * @param[in] section      セクション名。
     * @param[in] key          キー名。
     * @param[in] defaultValue 見つからない・解釈できない場合に返す値。
     * @return 設定値。
     */
    int getInt(std::string_view section, std::string_view key, int defaultValue) const;

    /**
     * @brief 色の設定値を取得する。
     * @param[in] section      セクション名。
     * @param[in] key          キー名("RRGGBB" または "#RRGGBB" 形式)。
     * @param[in] defaultValue 見つからない・解釈できない場合に返す値。
     * @return 0xRRGGBB 形式の色値。
     */
    uint32_t getColorRGB(std::string_view section, std::string_view key,
                         uint32_t defaultValue) const;

private:
    std::unordered_map<std::string, Section> sections_;
};

} // namespace blinker
