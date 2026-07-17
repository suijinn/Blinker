#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "core/command.h"

namespace blinker {

// プラットフォーム非依存のキーコード。
// 印字キー('0'-'9', 'A'-'Z')は ASCII 値と同値で、Windows の VK_* コードとも一致する。
// 例: KeyCode{'A'} で A キーを表す。
enum class KeyCode : uint16_t {
    None = 0,
    Left = 0x100, Right, Up, Down,
    Home, End, PageUp, PageDown,
    Space, Enter, Escape, Backspace, Delete, Tab, Insert,
    Plus, Minus,  // メインキー・テンキーの +/- を正規化したもの
    F1 = 0x200, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
};

struct KeyChord {
    KeyCode key = KeyCode::None;
    bool ctrl = false;
    bool shift = false;
    bool alt = false;

    bool operator==(const KeyChord&) const = default;
};

struct KeyChordHash {
    size_t operator()(const KeyChord& c) const {
        return static_cast<size_t>(c.key) | (c.ctrl ? 0x10000u : 0u) |
               (c.shift ? 0x20000u : 0u) | (c.alt ? 0x40000u : 0u);
    }
};

// キー→コマンドの対応表。デフォルト表を基点に blinker.ini の [keys] で上書きできる。
class Keymap {
public:
    static Keymap defaults();

    Command find(const KeyChord& chord) const;
    void bind(const KeyChord& chord, Command cmd);
    void unbindCommand(Command cmd);

    // "Ctrl+O" "Shift+R" "Right" "F11" "+" 形式(大文字小文字は区別しない)
    static std::optional<KeyChord> parseChord(std::string_view text);

    // [keys] セクション(コマンド名 = "Key1, Key2")を適用する。
    // 記述されたコマンドは既存バインドをすべて置き換える。
    void applyConfig(const std::unordered_map<std::string, std::string>& keysSection);

private:
    std::unordered_map<KeyChord, Command, KeyChordHash> bindings_;
};

// ini 用コマンド名("next" 等)からの変換。未知の名前は Command::None。
Command commandFromName(std::string_view name);

} // namespace blinker
