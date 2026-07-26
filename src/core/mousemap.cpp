#include "core/mousemap.h"

#include <algorithm>
#include <array>

#include "core/keymap.h"  // commandFromName([keys] と同じコマンド名を使う)
#include "core/str_util.h"

namespace blinker {
namespace {

struct InputName {
    std::string_view name;
    MouseInput input;
};

// ini で受け付ける表記(小文字で比較する)。別名も並べてよい
constexpr std::array kInputNames = {
    InputName{"middle", MouseInput::Middle},
    InputName{"x1", MouseInput::X1},
    InputName{"x2", MouseInput::X2},
    InputName{"doubleclick", MouseInput::DoubleClick},
    InputName{"double_click", MouseInput::DoubleClick},
    InputName{"wheelup", MouseInput::WheelUp},
    InputName{"wheeldown", MouseInput::WheelDown},
    InputName{"wheelleft", MouseInput::WheelLeft},
    InputName{"wheelright", MouseInput::WheelRight},
};

// 表示用の表記(kInputNames は別名を複数持つため、逆引きの正はこちら)。
// ここの表記は必ず parseChord が受け付けるものにすること(往復を単体テストで検証している)
constexpr std::array kInputDisplayNames = {
    InputName{"Middle", MouseInput::Middle},
    InputName{"X1", MouseInput::X1},
    InputName{"X2", MouseInput::X2},
    InputName{"DoubleClick", MouseInput::DoubleClick},
    InputName{"WheelUp", MouseInput::WheelUp},
    InputName{"WheelDown", MouseInput::WheelDown},
    InputName{"WheelLeft", MouseInput::WheelLeft},
    InputName{"WheelRight", MouseInput::WheelRight},
};

// 操作一覧に出す日本語表記(ini には書けない。書ける表記は kInputDisplayNames のほう)。
// サイドバー(操作一覧モードは 300px)に収まる長さにすること。既定の割り当てでは
// 「前 / 次の画像」の行に 3 つ並ぶため、ここが長いと右端で切れる
constexpr std::array kInputJapaneseNames = {
    InputName{"中ボタン", MouseInput::Middle},
    InputName{"サイド(戻る)", MouseInput::X1},
    InputName{"サイド(進む)", MouseInput::X2},
    InputName{"ダブルクリック", MouseInput::DoubleClick},
    InputName{"ホイール↑", MouseInput::WheelUp},
    InputName{"ホイール↓", MouseInput::WheelDown},
    InputName{"チルト←", MouseInput::WheelLeft},
    InputName{"チルト→", MouseInput::WheelRight},
};

// 修飾キーの有無を「修飾なしが先」になる整数へ畳む(chordsFor の並び順の安定化用)
unsigned modifierRank(const MouseChord& chord) {
    return (chord.ctrl ? 4u : 0u) | (chord.shift ? 2u : 0u) | (chord.alt ? 1u : 0u);
}

// 修飾キーの接頭辞("Ctrl+" 等)を組み立てる
std::string modifierPrefix(const MouseChord& chord) {
    std::string result;
    if (chord.ctrl) result += "Ctrl+";
    if (chord.shift) result += "Shift+";
    if (chord.alt) result += "Alt+";
    return result;
}

} // namespace

Mousemap Mousemap::defaults() {
    Mousemap mm;
    // サイドボタンはブラウザの戻る / 進むと同じ向きに合わせる(親指だけで送れる)
    mm.bind({MouseInput::X1}, Command::PrevImage);
    mm.bind({MouseInput::X2}, Command::NextImage);
    // 素のホイールはズーム(未割り当てのまま App::onWheel が受け持つ)なので、
    // どのマウスでも使える遷移は Ctrl 付きに置く
    mm.bind({MouseInput::WheelUp, true}, Command::PrevImage);
    mm.bind({MouseInput::WheelDown, true}, Command::NextImage);
    // 水平ホイールは衝突相手が無いので修飾キーなしで遷移に使う
    mm.bind({MouseInput::WheelLeft}, Command::PrevImage);
    mm.bind({MouseInput::WheelRight}, Command::NextImage);
    // 中ボタン・ダブルクリックは既定では割り当てない(誤爆したときに何が起きたか
    // 分かりにくい。ダブルクリックはテキスト注釈の再編集にも使われる)
    return mm;
}

Command Mousemap::find(const MouseChord& chord) const {
    const auto it = bindings_.find(chord);
    return it == bindings_.end() ? Command::None : it->second;
}

void Mousemap::bind(const MouseChord& chord, Command cmd) {
    if (chord.input == MouseInput::None || cmd == Command::None) return;
    bindings_[chord] = cmd;
}

void Mousemap::unbindCommand(Command cmd) {
    std::erase_if(bindings_, [cmd](const auto& kv) { return kv.second == cmd; });
}

std::string Mousemap::chordToString(const MouseChord& chord) {
    if (chord.input == MouseInput::None) return {};
    for (const auto& e : kInputDisplayNames) {
        if (e.input == chord.input) return modifierPrefix(chord) + std::string(e.name);
    }
    return {};
}

std::string Mousemap::chordToDisplayString(const MouseChord& chord) {
    if (chord.input == MouseInput::None) return {};
    for (const auto& e : kInputJapaneseNames) {
        if (e.input == chord.input) return modifierPrefix(chord) + std::string(e.name);
    }
    return {};
}

std::vector<MouseChord> Mousemap::chordsFor(Command cmd) const {
    std::vector<MouseChord> result;
    for (const auto& [chord, bound] : bindings_) {
        if (bound == cmd) result.push_back(chord);
    }
    // bindings_ は unordered_map なので、表示順が実行ごとに変わらないよう明示的に並べる
    std::sort(result.begin(), result.end(), [](const MouseChord& a, const MouseChord& b) {
        const unsigned ra = modifierRank(a);
        const unsigned rb = modifierRank(b);
        if (ra != rb) return ra < rb;
        return static_cast<uint16_t>(a.input) < static_cast<uint16_t>(b.input);
    });
    return result;
}

std::optional<MouseChord> Mousemap::parseChord(std::string_view text) {
    const std::string lower = toLower(trim(text));
    if (lower.empty()) return std::nullopt;

    MouseChord chord;
    std::string_view rest = lower;
    while (true) {
        const size_t plus = rest.find('+');
        if (plus == std::string_view::npos) break;
        const std::string_view mod = rest.substr(0, plus);
        if (mod == "ctrl" || mod == "control") {
            chord.ctrl = true;
        } else if (mod == "shift") {
            chord.shift = true;
        } else if (mod == "alt") {
            chord.alt = true;
        } else {
            return std::nullopt;
        }
        rest.remove_prefix(plus + 1);
    }
    for (const auto& e : kInputNames) {
        if (e.name == rest) {
            chord.input = e.input;
            return chord;
        }
    }
    return std::nullopt;
}

void Mousemap::applyConfig(const std::unordered_map<std::string, std::string>& mouseSection) {
    for (const auto& [name, value] : mouseSection) {
        // コマンド名でないキー(swap_buttons など)はここでは扱わない
        const Command cmd = commandFromName(name);
        if (cmd == Command::None) continue;
        unbindCommand(cmd);
        // カンマ区切りで複数の操作を許可
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

int consumeWheelSteps(float& accum, const float notches) {
    if (notches == 0.0f) return 0;
    // 逆方向へ回したときに、それまでの貯金で 1 回目が飲まれないようにする
    if ((accum > 0.0f && notches < 0.0f) || (accum < 0.0f && notches > 0.0f)) accum = 0.0f;
    accum += notches;
    const int steps = static_cast<int>(accum);  // 0 方向への切り捨て
    accum -= static_cast<float>(steps);
    return steps;
}

} // namespace blinker
