#include "core/config.h"

#include <charconv>
#include <fstream>
#include <sstream>

#include "core/str_util.h"

namespace blinker {

Config Config::loadFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return parse(buffer.str());
}

Config Config::parse(std::string_view text) {
    Config config;
    std::string sectionName;
    std::istringstream stream{std::string(text)};
    std::string rawLine;
    while (std::getline(stream, rawLine)) {
        const std::string_view line = trim(rawLine);
        if (line.empty() || line.front() == '#' || line.front() == ';') continue;
        if (line.front() == '[' && line.back() == ']') {
            sectionName = toLower(trim(line.substr(1, line.size() - 2)));
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string_view::npos) continue;
        const std::string key = toLower(trim(line.substr(0, eq)));
        if (key.empty()) continue;
        config.sections_[sectionName][key] = std::string(trim(line.substr(eq + 1)));
    }
    return config;
}

const Config::Section& Config::section(std::string_view name) const {
    static const Section kEmpty;
    const auto it = sections_.find(toLower(name));
    return it == sections_.end() ? kEmpty : it->second;
}

std::string Config::get(std::string_view sectionName, std::string_view key,
                        std::string defaultValue) const {
    const Section& s = section(sectionName);
    const auto it = s.find(toLower(key));
    return it == s.end() ? std::move(defaultValue) : it->second;
}

bool Config::getBool(std::string_view sectionName, std::string_view key,
                     bool defaultValue) const {
    const std::string v = toLower(get(sectionName, key));
    if (v == "true" || v == "1" || v == "yes" || v == "on") return true;
    if (v == "false" || v == "0" || v == "no" || v == "off") return false;
    return defaultValue;
}

int Config::getInt(std::string_view sectionName, std::string_view key, int defaultValue) const {
    const std::string v = get(sectionName, key);
    int out = 0;
    const auto [ptr, ec] = std::from_chars(v.data(), v.data() + v.size(), out);
    return (ec == std::errc{} && ptr == v.data() + v.size() && !v.empty()) ? out : defaultValue;
}

uint32_t Config::getColorRGB(std::string_view sectionName, std::string_view key,
                             uint32_t defaultValue) const {
    const std::string v = get(sectionName, key);
    std::string_view s = v;
    if (!s.empty() && s.front() == '#') s.remove_prefix(1);
    if (s.size() != 6) return defaultValue;
    uint32_t out = 0;
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out, 16);
    return (ec == std::errc{} && ptr == s.data() + s.size()) ? out : defaultValue;
}

} // namespace blinker
