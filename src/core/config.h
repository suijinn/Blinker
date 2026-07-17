#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>

namespace blinker {

// blinker.ini の読み込み。[section] / key = value / # ; コメントだけの最小 ini。
// セクション名・キー名は小文字に正規化して保持する。
class Config {
public:
    using Section = std::unordered_map<std::string, std::string>;

    static Config loadFile(const std::filesystem::path& path);  // 存在しなければ空
    static Config parse(std::string_view text);

    const Section& section(std::string_view name) const;
    std::string get(std::string_view section, std::string_view key,
                    std::string defaultValue = {}) const;
    bool getBool(std::string_view section, std::string_view key, bool defaultValue) const;
    int getInt(std::string_view section, std::string_view key, int defaultValue) const;
    // "RRGGBB" / "#RRGGBB" → 0xRRGGBB
    uint32_t getColorRGB(std::string_view section, std::string_view key,
                         uint32_t defaultValue) const;

private:
    std::unordered_map<std::string, Section> sections_;
};

} // namespace blinker
