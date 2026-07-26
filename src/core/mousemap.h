#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/command.h"

/**
 * @file mousemap.h
 * @brief マウス操作の正規化表現と、マウス操作 → Command の対応表。
 */

namespace blinker {

/**
 * @brief コマンドを割り当てられるマウス操作。
 *
 * 左右ボタンの単独クリック・ドラッグはここに含めない。パン・編集ドラッグ・
 * メニューで埋まっており、割り当ての対象にすると App::mouseRole
 * (`[mouse] swap_buttons`)との二重管理になるため。
 */
enum class MouseInput : uint16_t {
    None = 0,
    Middle,       ///< 中ボタン(ホイールの押し込み)
    X1,           ///< サイドボタン 1(ブラウザの「戻る」)
    X2,           ///< サイドボタン 2(ブラウザの「進む」)
    DoubleClick,  ///< 左ダブルクリック(テキスト注釈の再編集にならなかった場合のみ)
    WheelUp,      ///< 垂直ホイールを奥へ
    WheelDown,    ///< 垂直ホイールを手前へ
    WheelLeft,    ///< 水平ホイール(チルト)を左へ
    WheelRight,   ///< 水平ホイール(チルト)を右へ
};

/// @brief 修飾キーを含む 1 回のマウス操作。
struct MouseChord {
    MouseInput input = MouseInput::None;  ///< 主となる操作
    bool ctrl = false;                    ///< Ctrl が押されているか
    bool shift = false;                   ///< Shift が押されているか
    bool alt = false;                     ///< Alt が押されているか

    /**
     * @brief 主操作と修飾キーがすべて一致するかを比較する。
     * @return 全メンバが等しければ true。
     */
    bool operator==(const MouseChord&) const = default;
};

/// @brief MouseChord を unordered_map のキーにするためのハッシュ関手。
struct MouseChordHash {
    /**
     * @brief MouseChord のハッシュ値を求める。
     * @param[in] c ハッシュ対象のマウス操作。
     * @return 操作の種別と修飾フラグを詰めたハッシュ値。
     */
    size_t operator()(const MouseChord& c) const {
        return static_cast<size_t>(c.input) | (c.ctrl ? 0x100u : 0u) |
               (c.shift ? 0x200u : 0u) | (c.alt ? 0x400u : 0u);
    }
};

/**
 * @brief マウス操作 → コマンドの対応表。
 *
 * Keymap と同じ形(デフォルト表を基点に blinker.ini の [mouse] で上書き)。
 *
 * @note ホイールでのズームはこの表に載せない。カーソル位置を基準に拡大縮小する
 *       ため Command::ZoomIn と等価にならず、「割り当てのないホイールの既定動作」
 *       として App::onWheel が受け持つ。この作りにより、ini で
 *       `next = WheelDown` と書けば素のホイールが遷移になり、既定の
 *       Ctrl+ホイールの割り当ては消えてズームへ落ちる(モード設定は不要)。
 */
class Mousemap {
public:
    /**
     * @brief 既定のマウスバインドを持つ Mousemap を作る。
     * @return デフォルト表が設定された Mousemap。
     */
    static Mousemap defaults();

    /**
     * @brief マウス操作に対応するコマンドを引く。
     * @param[in] chord 検索するマウス操作。
     * @return 対応するコマンド。バインドがなければ Command::None。
     */
    Command find(const MouseChord& chord) const;

    /**
     * @brief マウス操作にコマンドを割り当てる(既存の割り当ては上書き)。
     * @param[in] chord 割り当て先のマウス操作。
     * @param[in] cmd   割り当てるコマンド。
     */
    void bind(const MouseChord& chord, Command cmd);

    /**
     * @brief 指定コマンドへのバインドをすべて解除する。
     * @param[in] cmd 解除するコマンド。
     */
    void unbindCommand(Command cmd);

    /**
     * @brief マウス操作の表記文字列を MouseChord へ解析する。
     * @param[in] text "X2" "Ctrl+WheelDown" "Middle" "DoubleClick" 形式
     *                 (大文字小文字は区別しない)。
     * @return 解析結果。認識できない表記なら std::nullopt。
     */
    static std::optional<MouseChord> parseChord(std::string_view text);

    /**
     * @brief MouseChord を表記文字列へ変換する(parseChord の逆)。
     * @param[in] chord 変換するマウス操作。
     * @return "X2" "Ctrl+WheelDown" 形式。input が None なら空文字列。
     * @note 戻り値はそのまま parseChord へ渡せる(blinker.ini の [mouse] に書ける表記)。
     */
    static std::string chordToString(const MouseChord& chord);

    /**
     * @brief MouseChord を操作一覧向けの日本語表記へ変換する。
     * @param[in] chord 変換するマウス操作。
     * @return "サイドボタン(進む)" "Ctrl+ホイール↓" 形式。input が None なら空文字列。
     */
    static std::string chordToDisplayString(const MouseChord& chord);

    /**
     * @brief 指定コマンドに割り当てられているマウス操作をすべて返す。
     * @param[in] cmd 検索するコマンド。
     * @return 割り当てられたマウス操作。バインドがなければ空。
     * @note 並びは修飾なし → Ctrl/Shift/Alt の順で安定(内部の格納順に依存しない)。
     */
    std::vector<MouseChord> chordsFor(Command cmd) const;

    /**
     * @brief blinker.ini の [mouse] セクションを適用する。
     *
     * 記述されたコマンドは既存バインドをすべて置き換える(Keymap::applyConfig と同じ規則)。
     * コマンド名として解釈できないキー(`swap_buttons` など)は無視する。
     *
     * @param[in] mouseSection キー → 値の対応表(コマンド名 → "X2, Ctrl+WheelDown")。
     */
    void applyConfig(const std::unordered_map<std::string, std::string>& mouseSection);

private:
    std::unordered_map<MouseChord, Command, MouseChordHash> bindings_;
};

/**
 * @brief ホイールの回転量を蓄積し、確定した段数を取り出す。
 *
 * 画像の遷移は離散なので、1 ノッチ未満ずつ通知される高精細ホイールや
 * タッチパッドでも取りこぼさないよう蓄積してから 1 段ずつ消費する。
 *
 * @param[in,out] accum   蓄積値(呼び出し側が保持する。消費分は差し引かれる)。
 * @param[in]     notches 今回の回転量(ノッチ単位。正で奥へ)。
 * @return 確定した段数(正で奥へ、負で手前へ)。
 * @note 前回と符号が反転した場合、蓄積値は捨ててから数え直す
 *       (逆方向へ回したときに 1 回目が飲まれないようにするため)。
 */
int consumeWheelSteps(float& accum, float notches);

} // namespace blinker
