#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "core/command.h"

/**
 * @file keymap.h
 * @brief キー入力の正規化表現と、キー → Command の対応表。
 */

namespace blinker {

/**
 * @brief プラットフォーム非依存のキーコード。
 *
 * 印字キー('0'-'9', 'A'-'Z')は ASCII 値と同値で、Windows の VK_* コードとも一致する。
 * 例: KeyCode{'A'} で A キーを表す。
 */
enum class KeyCode : uint16_t {
    None = 0,
    Left = 0x100, Right, Up, Down,
    Home, End, PageUp, PageDown,
    Space, Enter, Escape, Backspace, Delete, Tab, Insert,
    /// メインキー・テンキーの +/- を正規化したもの
    Plus, Minus,
    F1 = 0x200, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
};

/// @brief 修飾キーを含む 1 回のキー入力。
struct KeyChord {
    KeyCode key = KeyCode::None;  ///< 主キー
    bool ctrl = false;            ///< Ctrl が押されているか
    bool shift = false;           ///< Shift が押されているか
    bool alt = false;             ///< Alt が押されているか

    /**
     * @brief 主キーと修飾キーがすべて一致するかを比較する。
     * @return 全メンバが等しければ true。
     */
    bool operator==(const KeyChord&) const = default;
};

/// @brief KeyChord を unordered_map のキーにするためのハッシュ関手。
struct KeyChordHash {
    /**
     * @brief KeyChord のハッシュ値を求める。
     * @param[in] c ハッシュ対象のキー入力。
     * @return キーコードと修飾フラグを詰めたハッシュ値。
     */
    size_t operator()(const KeyChord& c) const {
        return static_cast<size_t>(c.key) | (c.ctrl ? 0x10000u : 0u) |
               (c.shift ? 0x20000u : 0u) | (c.alt ? 0x40000u : 0u);
    }
};

/**
 * @brief キー → コマンドの対応表。
 *
 * デフォルト表を基点に blinker.ini の [keys] で上書きできる。
 */
class Keymap {
public:
    /**
     * @brief 既定のキーバインドを持つ Keymap を作る。
     * @return デフォルト表が設定された Keymap。
     */
    static Keymap defaults();

    /**
     * @brief キー入力に対応するコマンドを引く。
     * @param[in] chord 検索するキー入力。
     * @return 対応するコマンド。バインドがなければ Command::None。
     */
    Command find(const KeyChord& chord) const;

    /**
     * @brief キー入力にコマンドを割り当てる(既存の割り当ては上書き)。
     * @param[in] chord 割り当て先のキー入力。
     * @param[in] cmd   割り当てるコマンド。
     */
    void bind(const KeyChord& chord, Command cmd);

    /**
     * @brief 指定コマンドへのバインドをすべて解除する。
     * @param[in] cmd 解除するコマンド。
     */
    void unbindCommand(Command cmd);

    /**
     * @brief キー表記文字列を KeyChord へ解析する。
     * @param[in] text "Ctrl+O" "Shift+R" "Right" "F11" "+" 形式(大文字小文字は区別しない)。
     * @return 解析結果。認識できない表記なら std::nullopt。
     */
    static std::optional<KeyChord> parseChord(std::string_view text);

    /**
     * @brief blinker.ini の [keys] セクションを適用する。
     *
     * 記述されたコマンドは既存バインドをすべて置き換える。
     *
     * @param[in] keysSection コマンド名 → "Key1, Key2" の対応表。
     */
    void applyConfig(const std::unordered_map<std::string, std::string>& keysSection);

private:
    std::unordered_map<KeyChord, Command, KeyChordHash> bindings_;
};

/**
 * @brief ini 用コマンド名から Command へ変換する。
 * @param[in] name コマンド名("next" 等)。
 * @return 対応するコマンド。未知の名前なら Command::None。
 */
Command commandFromName(std::string_view name);

} // namespace blinker
