#include "core/keymap.h"

#include <array>
#include <charconv>
#include <vector>

#include "core/str_util.h"

namespace blinker {
namespace {

struct CommandName {
    std::string_view name;
    Command cmd;
};

constexpr std::array kCommandNames = {
    CommandName{"next", Command::NextImage},
    CommandName{"prev", Command::PrevImage},
    CommandName{"first", Command::FirstImage},
    CommandName{"last", Command::LastImage},
    CommandName{"zoom_in", Command::ZoomIn},
    CommandName{"zoom_out", Command::ZoomOut},
    CommandName{"fit", Command::ZoomFit},
    CommandName{"actual_size", Command::ZoomActual},
    CommandName{"pan_left", Command::PanLeft},
    CommandName{"pan_right", Command::PanRight},
    CommandName{"pan_up", Command::PanUp},
    CommandName{"pan_down", Command::PanDown},
    CommandName{"rotate_cw", Command::RotateCW},
    CommandName{"rotate_ccw", Command::RotateCCW},
    CommandName{"fullscreen", Command::ToggleFullscreen},
    CommandName{"open", Command::OpenFile},
    CommandName{"copy_image", Command::CopyImage},
    CommandName{"copy_path", Command::CopyPath},
    CommandName{"paste", Command::PasteImage},
    CommandName{"save_as", Command::SaveImageAs},
    CommandName{"undo", Command::Undo},
    CommandName{"delete_annotation", Command::DeleteAnnotation},
    // ツール切り替えは既定のキーを持たない(blinker.ini の [keys] で割り当てる)
    CommandName{"tool_crop", Command::SelectToolCrop},
    CommandName{"tool_rect", Command::SelectToolRect},
    CommandName{"tool_ellipse", Command::SelectToolEllipse},
    CommandName{"tool_arrow", Command::SelectToolArrow},
    CommandName{"tool_line", Command::SelectToolLine},
    CommandName{"tool_text", Command::SelectToolText},
    CommandName{"sidebar", Command::ToggleSidebar},
    CommandName{"statusbar", Command::ToggleStatusBar},
    CommandName{"escape", Command::Escape},
    CommandName{"quit", Command::Quit},
};

struct KeyName {
    std::string_view name;
    KeyCode key;
};

constexpr std::array kKeyNames = {
    KeyName{"left", KeyCode::Left},
    KeyName{"right", KeyCode::Right},
    KeyName{"up", KeyCode::Up},
    KeyName{"down", KeyCode::Down},
    KeyName{"home", KeyCode::Home},
    KeyName{"end", KeyCode::End},
    KeyName{"pageup", KeyCode::PageUp},
    KeyName{"pagedown", KeyCode::PageDown},
    KeyName{"space", KeyCode::Space},
    KeyName{"enter", KeyCode::Enter},
    KeyName{"return", KeyCode::Enter},
    KeyName{"esc", KeyCode::Escape},
    KeyName{"escape", KeyCode::Escape},
    KeyName{"backspace", KeyCode::Backspace},
    KeyName{"delete", KeyCode::Delete},
    KeyName{"del", KeyCode::Delete},
    KeyName{"tab", KeyCode::Tab},
    KeyName{"insert", KeyCode::Insert},
    KeyName{"plus", KeyCode::Plus},
    KeyName{"+", KeyCode::Plus},
    KeyName{"minus", KeyCode::Minus},
    KeyName{"-", KeyCode::Minus},
};

} // namespace

Command commandFromName(std::string_view name) {
    const std::string lower = toLower(trim(name));
    for (const auto& e : kCommandNames) {
        if (e.name == lower) return e.cmd;
    }
    return Command::None;
}

Keymap Keymap::defaults() {
    Keymap km;
    auto b = [&km](KeyCode key, Command cmd, bool ctrl = false, bool shift = false) {
        km.bind({key, ctrl, shift, false}, cmd);
    };
    b(KeyCode::Right, Command::NextImage);
    b(KeyCode::Down, Command::NextImage);
    b(KeyCode::Space, Command::NextImage);
    b(KeyCode::PageDown, Command::NextImage);
    b(KeyCode::Left, Command::PrevImage);
    b(KeyCode::Up, Command::PrevImage);
    b(KeyCode::Backspace, Command::PrevImage);
    b(KeyCode::PageUp, Command::PrevImage);
    b(KeyCode::Home, Command::FirstImage);
    b(KeyCode::End, Command::LastImage);
    b(KeyCode::Plus, Command::ZoomIn);
    b(KeyCode::Minus, Command::ZoomOut);
    b(KeyCode{'0'}, Command::ZoomFit);
    b(KeyCode{'1'}, Command::ZoomActual);
    b(KeyCode::Left, Command::PanLeft, true);
    b(KeyCode::Right, Command::PanRight, true);
    b(KeyCode::Up, Command::PanUp, true);
    b(KeyCode::Down, Command::PanDown, true);
    b(KeyCode{'R'}, Command::RotateCW);
    b(KeyCode{'R'}, Command::RotateCCW, false, true);
    b(KeyCode::F11, Command::ToggleFullscreen);
    b(KeyCode::Enter, Command::ToggleFullscreen);
    b(KeyCode{'O'}, Command::OpenFile, true);
    b(KeyCode{'C'}, Command::CopyImage, true);
    b(KeyCode{'C'}, Command::CopyPath, true, true);
    b(KeyCode{'V'}, Command::PasteImage, true);
    b(KeyCode{'S'}, Command::SaveImageAs, true);
    b(KeyCode{'Z'}, Command::Undo, true);
    b(KeyCode::Delete, Command::DeleteAnnotation);
    b(KeyCode{'B'}, Command::ToggleSidebar, true);  // Ctrl+B
    b(KeyCode{'B'}, Command::ToggleStatusBar);
    b(KeyCode::Escape, Command::Escape);
    b(KeyCode{'Q'}, Command::Quit);
    b(KeyCode{'W'}, Command::Quit, true);
    return km;
}

Command Keymap::find(const KeyChord& chord) const {
    const auto it = bindings_.find(chord);
    return it == bindings_.end() ? Command::None : it->second;
}

void Keymap::bind(const KeyChord& chord, Command cmd) {
    if (chord.key == KeyCode::None || cmd == Command::None) return;
    bindings_[chord] = cmd;
}

void Keymap::unbindCommand(Command cmd) {
    std::erase_if(bindings_, [cmd](const auto& kv) { return kv.second == cmd; });
}

std::optional<KeyChord> Keymap::parseChord(std::string_view text) {
    const std::string lower = toLower(trim(text));

    // '+' で分割する。ただし直前が区切りの '+' はキー名としての "+" とみなす("ctrl++" 等)
    std::vector<std::string> tokens;
    std::string current;
    for (const char c : lower) {
        if (c == '+' && !current.empty()) {
            tokens.push_back(current);
            current.clear();
        } else if (c == '+' && current.empty()) {
            current = "+";
        } else {
            current += c;
        }
    }
    if (!current.empty()) tokens.push_back(current);
    if (tokens.empty()) return std::nullopt;

    KeyChord chord;
    for (size_t i = 0; i + 1 < tokens.size(); ++i) {
        const std::string& mod = tokens[i];
        if (mod == "ctrl" || mod == "control") {
            chord.ctrl = true;
        } else if (mod == "shift") {
            chord.shift = true;
        } else if (mod == "alt") {
            chord.alt = true;
        } else {
            return std::nullopt;
        }
    }

    const std::string& keyName = tokens.back();
    for (const auto& e : kKeyNames) {
        if (e.name == keyName) {
            chord.key = e.key;
            return chord;
        }
    }
    if (keyName.size() >= 2 && keyName[0] == 'f') {
        int n = 0;
        const char* first = keyName.data() + 1;
        const char* last = keyName.data() + keyName.size();
        const auto [ptr, ec] = std::from_chars(first, last, n);
        if (ec == std::errc{} && ptr == last && n >= 1 && n <= 12) {
            chord.key = static_cast<KeyCode>(static_cast<uint16_t>(KeyCode::F1) + n - 1);
            return chord;
        }
    }
    if (keyName.size() == 1) {
        const char c = keyName[0];
        if (c >= 'a' && c <= 'z') {
            chord.key = static_cast<KeyCode>(c - 'a' + 'A');
            return chord;
        }
        if (c >= '0' && c <= '9') {
            chord.key = static_cast<KeyCode>(c);
            return chord;
        }
    }
    return std::nullopt;
}

void Keymap::applyConfig(const std::unordered_map<std::string, std::string>& keysSection) {
    for (const auto& [name, value] : keysSection) {
        const Command cmd = commandFromName(name);
        if (cmd == Command::None) continue;
        unbindCommand(cmd);
        // カンマ区切りで複数キーを許可
        std::string_view rest = value;
        while (!rest.empty()) {
            const size_t comma = rest.find(',');
            const std::string_view token = trim(rest.substr(0, comma));
            if (!token.empty()) {
                if (const auto chord = parseChord(token)) bind(*chord, cmd);
            }
            if (comma == std::string_view::npos) break;
            rest.remove_prefix(comma + 1);
        }
    }
}

} // namespace blinker
