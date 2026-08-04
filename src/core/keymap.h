#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

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
 * @brief キーバインドが効く文脈。
 *
 * コマンドはどちらか一方にだけ属する。同じキー(矢印など)へ文脈ごとに別の
 * コマンドを割り当てられるようにするため、対応表を文脈ごとに分けて持つ。
 */
enum class KeyScope {
    Global,     ///< 常に効く(通常のキーバインド)
    Selection,  ///< 注釈オブジェクトを選択している間だけ効き、Global より優先される
};

/**
 * @brief コマンドが属する文脈を返す。
 * @param[in] cmd 調べるコマンド。
 * @return オブジェクト選択中だけ効くコマンドなら KeyScope::Selection、他は KeyScope::Global。
 */
KeyScope keyScopeOf(Command cmd);

/**
 * @brief キー → コマンドの対応表。
 *
 * デフォルト表を基点に blinker.ini の [keys] で上書きできる。
 * 対応表は文脈ごとに別インスタンスとして持つ(defaults / selectionDefaults)。
 */
class Keymap {
public:
    /**
     * @brief 既定のキーバインドを持つ Keymap を作る(KeyScope::Global 用)。
     * @return デフォルト表が設定された Keymap。
     */
    static Keymap defaults();

    /**
     * @brief オブジェクト選択中だけ効く既定のキーバインドを持つ Keymap を作る。
     *
     * 矢印キーでの移動だけを持つ。ここに無いキー(Shift+矢印 のページ送り、
     * Ctrl+矢印 のパン、PageUp/PageDown の画像遷移など)は選択中でも
     * KeyScope::Global の表がそのまま受け持つ。
     *
     * @return KeyScope::Selection のデフォルト表が設定された Keymap。
     */
    static Keymap selectionDefaults();

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
     * @brief KeyChord をキー表記文字列へ変換する(parseChord の逆)。
     * @param[in] chord 変換するキー入力。
     * @return "Ctrl+O" "Shift+R" "Right" "F11" "+" 形式。key が None なら空文字列。
     * @note 戻り値はそのまま parseChord へ渡せる(blinker.ini の [keys] に書ける表記)。
     */
    static std::string chordToString(const KeyChord& chord);

    /**
     * @brief 指定コマンドに割り当てられているキーをすべて返す。
     * @param[in] cmd 検索するコマンド。
     * @return 割り当てられたキー入力。バインドがなければ空。
     * @note 並びは修飾なし → Ctrl/Shift/Alt の順で安定(内部の格納順に依存しない)。
     */
    std::vector<KeyChord> chordsFor(Command cmd) const;

    /**
     * @brief blinker.ini の [keys] セクションを適用する。
     *
     * 記述されたコマンドは既存バインドをすべて置き換える。
     * ini のセクションは 1 つで、文脈の違う対応表それぞれに同じものを渡す
     * (自分の scope に属さないコマンドの記述は読み飛ばすので、
     * 利用者はコマンド名だけを書けばよく、どの表に入るかを意識しなくてよい)。
     *
     * @param[in] keysSection コマンド名 → "Key1, Key2" の対応表。
     * @param[in] scope       この対応表が受け持つ文脈。keyScopeOf が一致する
     *                        コマンドの記述だけを適用する。
     */
    void applyConfig(const std::unordered_map<std::string, std::string>& keysSection,
                     KeyScope scope = KeyScope::Global);

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
