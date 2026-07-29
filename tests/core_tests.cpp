// core 層の単体テスト。フレームワーク不使用の軽量 CHECK マクロで検証する。
// 実行: build/<preset>/tests/core_tests.exe (ctest からも起動される)

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <format>
#include <iostream>
#include <mutex>
#include <set>

#include "core/annotation_edit.h"
#include "core/app.h"
#include "core/config.h"
#include "core/dib.h"
#include "core/edit.h"
#include "core/edit_history.h"
#include "core/exif.h"
#include "core/geometry.h"
#include "core/image_scale.h"
#include "core/help.h"
#include "core/image_cache.h"
#include "core/image_list.h"
#include "core/keymap.h"
#include "core/mousemap.h"
#include "core/nav_arrows.h"
#include "core/ocr_service.h"
#include "core/ocr_text.h"
#include "core/pixel_convert.h"
#include "core/pointer_state.h"
#include "core/print_layout.h"
#include "core/scan_service.h"
#include "core/sidebar_state.h"
#include "core/sort_order.h"
#include "core/str_util.h"
#include "core/text_edit.h"
#include "core/text_style.h"
#include "core/unicode.h"
#include "core/version.h"
#include "core/viewport.h"

namespace {

int g_failures = 0;

void checkImpl(bool ok, const char* expr, const char* file, int line) {
    if (!ok) {
        ++g_failures;
        std::cout << "FAIL " << file << ":" << line << "  " << expr << "\n";
    }
}

#define CHECK(cond) checkImpl((cond), #cond, __FILE__, __LINE__)

bool nearly(float a, float b, float eps = 0.001f) {
    return std::fabs(a - b) <= eps;
}

using namespace blinker;

void testMatrix() {
    // 平行移動→スケールの合成 (行ベクトル規約: 左から順に適用)
    const Matrix3x2 m = Matrix3x2::translation(10, 20) * Matrix3x2::scale(2);
    const Point p = m.apply({1, 1});
    CHECK(nearly(p.x, 22));
    CHECK(nearly(p.y, 42));

    // 時計回り90度: (1,0) → (0,1)  (Y下向きスクリーン座標系)
    const Point r = Matrix3x2::rotation90(1).apply({1, 0});
    CHECK(nearly(r.x, 0));
    CHECK(nearly(r.y, 1));

    // 4回転で元に戻る
    const Point r4 = Matrix3x2::rotation90(4).apply({3, 7});
    CHECK(nearly(r4.x, 3));
    CHECK(nearly(r4.y, 7));

    // 逆変換との往復で元に戻る
    const Matrix3x2 t =
        Matrix3x2::translation(5, -3) * Matrix3x2::scale(2) * Matrix3x2::rotation90(1);
    const Point back = t.inverted().apply(t.apply({12, 34}));
    CHECK(nearly(back.x, 12));
    CHECK(nearly(back.y, 34));
}

void testViewportFit() {
    Viewport vp;
    vp.setWindowSize({800, 600});
    vp.setImage({1600, 600});
    CHECK(vp.fitMode());
    CHECK(nearly(vp.zoom(), 0.5f));  // 幅方向に律速

    // 画像中心はウィンドウ中心へ
    const Point center = vp.imageToScreen().apply({800, 300});
    CHECK(nearly(center.x, 400));
    CHECK(nearly(center.y, 300));

    // 小さい画像は既定では等倍のまま(拡大しない)
    vp.setImage({100, 100});
    CHECK(nearly(vp.zoom(), 1.0f));

    // fit_upscale 有効なら拡大する
    vp.setFitUpscale(true);
    CHECK(nearly(vp.zoom(), 6.0f));
    vp.setFitUpscale(false);

    // 回転すると縦横が入れ替わってフィット率が変わる
    vp.setImage({1600, 600});
    vp.rotate(1);
    CHECK(vp.rotationDegrees() == 90);
    CHECK(nearly(vp.zoom(), 600.0f / 1600.0f));  // 高さ方向に律速
}

void testViewportZoomAt() {
    Viewport vp;
    vp.setWindowSize({800, 600});
    vp.setImage({1600, 1200});
    CHECK(nearly(vp.zoom(), 0.5f));

    // カーソル位置 (200,150) 直下の画像点はズーム後も同じスクリーン位置に留まる
    const Point imagePoint{400, 300};  // ズーム前に (200,150) に見えている点
    const Point before = vp.imageToScreen().apply(imagePoint);
    CHECK(nearly(before.x, 200));
    CHECK(nearly(before.y, 150));

    vp.zoomAt(2.0f, {200, 150});
    CHECK(nearly(vp.zoom(), 1.0f));
    CHECK(!vp.fitMode());
    const Point after = vp.imageToScreen().apply(imagePoint);
    CHECK(nearly(after.x, 200));
    CHECK(nearly(after.y, 150));

    // screenToImage は imageToScreen の逆変換
    const Point roundTrip = vp.screenToImage().apply(after);
    CHECK(nearly(roundTrip.x, imagePoint.x));
    CHECK(nearly(roundTrip.y, imagePoint.y));

    // パンは画像端がウィンドウ内に収まる範囲へクランプされる
    vp.panBy(100000, 100000);
    const Point corner = vp.imageToScreen().apply({0, 0});  // 画像左上
    CHECK(nearly(corner.x, 0));  // 限界までパンすると左上が (0,0) に一致
    CHECK(nearly(corner.y, 0));

    // 画像がウィンドウより小さい場合はパン不可(中央固定)
    vp.setImage({100, 100});
    vp.actualSize();
    vp.panBy(500, 500);
    const Point smallCenter = vp.imageToScreen().apply({50, 50});
    CHECK(nearly(smallCenter.x, 400));
    CHECK(nearly(smallCenter.y, 300));
}

void testKeymap() {
    const Keymap km = Keymap::defaults();
    CHECK(km.find({KeyCode::Right}) == Command::NextImage);
    CHECK(km.find({KeyCode::Down}) == Command::NextImage);
    CHECK(km.find({KeyCode::Up}) == Command::PrevImage);
    CHECK(km.find({KeyCode{'W'}, true}) == Command::Quit);  // Ctrl+W
    CHECK(km.find({KeyCode::Right, true}) == Command::PanRight);  // Ctrl+Right
    CHECK(km.find({KeyCode{'R'}}) == Command::RotateCW);
    CHECK(km.find({KeyCode{'R'}, false, true}) == Command::RotateCCW);  // Shift+R
    CHECK(km.find({KeyCode{'Z'}}) == Command::None);

    auto chord = Keymap::parseChord("Ctrl+O");
    CHECK(chord && chord->key == KeyCode{'O'} && chord->ctrl && !chord->shift);
    chord = Keymap::parseChord("shift+r");
    CHECK(chord && chord->key == KeyCode{'R'} && chord->shift);
    chord = Keymap::parseChord("F11");
    CHECK(chord && chord->key == KeyCode::F11);
    CHECK(km.find({KeyCode{'C'}, true}) == Command::CopyImage);         // Ctrl+C
    CHECK(km.find({KeyCode{'C'}, true, true}) == Command::CopyPath);    // Shift+Ctrl+C
    CHECK(km.find({KeyCode{'C'}, false, true}) == Command::CopyFile);   // Shift+C
    CHECK(km.find({KeyCode{'V'}, true}) == Command::PasteImage);        // Ctrl+V
    CHECK(km.find({KeyCode{'S'}, true}) == Command::SaveImage);          // Ctrl+S
    CHECK(km.find({KeyCode{'S'}, true, true}) == Command::SaveImageAs);  // Shift+Ctrl+S
    CHECK(km.find({KeyCode{'P'}, true}) == Command::PrintImage);         // Ctrl+P
    CHECK(km.find({KeyCode{'B'}}) == Command::ToggleStatusBar);
    CHECK(km.find({KeyCode{'B'}, true}) == Command::ToggleSidebar);     // Ctrl+B
    CHECK(commandFromName("copy_file") == Command::CopyFile);
    CHECK(commandFromName("paste") == Command::PasteImage);
    CHECK(commandFromName("save") == Command::SaveImage);
    CHECK(commandFromName("save_as") == Command::SaveImageAs);
    CHECK(commandFromName("print") == Command::PrintImage);
    CHECK(commandFromName("sidebar") == Command::ToggleSidebar);
    chord = Keymap::parseChord("+");
    CHECK(chord && chord->key == KeyCode::Plus);
    chord = Keymap::parseChord("Ctrl++");
    CHECK(chord && chord->key == KeyCode::Plus && chord->ctrl);
    CHECK(!Keymap::parseChord("foo"));
    CHECK(!Keymap::parseChord(""));

    // ini による上書き: 記述したコマンドの既存バインドは置き換わる
    Keymap custom = Keymap::defaults();
    custom.applyConfig({{"next", "N, Tab"}});
    CHECK(custom.find({KeyCode{'N'}}) == Command::NextImage);
    CHECK(custom.find({KeyCode::Tab}) == Command::NextImage);
    CHECK(custom.find({KeyCode::Right}) == Command::None);       // 置き換え済み
    CHECK(custom.find({KeyCode::Left}) == Command::PrevImage);   // 他コマンドは無傷

    // 編集ツールの切り替えは既定のキーを持たず、ini で割り当てる
    CHECK(commandFromName("tool_arrow") == Command::SelectToolArrow);
    CHECK(commandFromName("tool_crop") == Command::SelectToolCrop);
    Keymap tools = Keymap::defaults();
    CHECK(tools.find({KeyCode{'A'}}) == Command::None);
    tools.applyConfig({{"tool_arrow", "A"}, {"tool_text", "T"}});
    CHECK(tools.find({KeyCode{'A'}}) == Command::SelectToolArrow);
    CHECK(tools.find({KeyCode{'T'}}) == Command::SelectToolText);

    // 操作一覧 (F1)
    CHECK(km.find({KeyCode::F1}) == Command::ToggleHelp);
    CHECK(commandFromName("help") == Command::ToggleHelp);
}

void testChordToString() {
    CHECK(Keymap::chordToString({KeyCode{'O'}, true}) == "Ctrl+O");
    CHECK(Keymap::chordToString({KeyCode{'C'}, true, true}) == "Ctrl+Shift+C");
    CHECK(Keymap::chordToString({KeyCode::Right}) == "Right");
    CHECK(Keymap::chordToString({KeyCode::PageDown}) == "PageDown");
    CHECK(Keymap::chordToString({KeyCode::F11}) == "F11");
    CHECK(Keymap::chordToString({KeyCode::Plus}) == "+");
    CHECK(Keymap::chordToString({KeyCode{'0'}}) == "0");
    CHECK(Keymap::chordToString({KeyCode::None}).empty());
    CHECK(Keymap::chordToString({KeyCode::Left, true, false, true}) == "Ctrl+Alt+Left");

    // 表示用のキー名がそのまま blinker.ini に書けること(表示表と解析表の drift 検出)。
    // 既定のバインドを総当たりで往復させる
    const Keymap km = Keymap::defaults();
    size_t roundTripped = 0;
    for (int cmd = 0; cmd <= static_cast<int>(Command::Quit); ++cmd) {
        for (const KeyChord& chord : km.chordsFor(static_cast<Command>(cmd))) {
            const std::string text = Keymap::chordToString(chord);
            CHECK(!text.empty());
            const auto parsed = Keymap::parseChord(text);
            CHECK(parsed && *parsed == chord);
            ++roundTripped;
        }
    }
    CHECK(roundTripped > 20);  // 既定表を素通りしていないことの歯止め

    // chordsFor の並びは修飾なしが先で、格納順(unordered_map)に依存しない
    const std::vector<KeyChord> pan = km.chordsFor(Command::PanLeft);
    CHECK(pan.size() == 1);
    CHECK((pan[0] == KeyChord{KeyCode::Left, true}));
    const std::vector<KeyChord> quit = km.chordsFor(Command::Quit);
    CHECK(quit.size() == 2);
    CHECK((quit[0] == KeyChord{KeyCode{'Q'}}));        // 修飾なしが先
    CHECK((quit[1] == KeyChord{KeyCode{'W'}, true}));  // Ctrl+W
    CHECK(km.chordsFor(Command::SelectToolArrow).empty());
}

void testMousemap() {
    const Mousemap mm = Mousemap::defaults();
    // 既定はサイドボタンと Ctrl+ホイール、水平ホイールで前後の画像
    CHECK(mm.find({MouseInput::X2}) == Command::NextImage);
    CHECK(mm.find({MouseInput::X1}) == Command::PrevImage);
    CHECK(mm.find({MouseInput::WheelDown, true}) == Command::NextImage);
    CHECK(mm.find({MouseInput::WheelUp, true}) == Command::PrevImage);
    CHECK(mm.find({MouseInput::WheelRight}) == Command::NextImage);
    CHECK(mm.find({MouseInput::WheelLeft}) == Command::PrevImage);
    // 素のホイールは未割り当て(App::onWheel がカーソル位置基準のズームに使う)
    CHECK(mm.find({MouseInput::WheelDown}) == Command::None);
    CHECK(mm.find({MouseInput::WheelUp}) == Command::None);
    // 中ボタン・ダブルクリックも既定では未割り当て
    CHECK(mm.find({MouseInput::Middle}) == Command::None);
    CHECK(mm.find({MouseInput::DoubleClick}) == Command::None);
    // 修飾キーが違えば別の操作
    CHECK(mm.find({MouseInput::X2, true}) == Command::None);

    // 表記の解析と往復(表示用の表記はそのまま ini に書ける)
    CHECK((Mousemap::parseChord("X2") == MouseChord{MouseInput::X2}));
    CHECK((Mousemap::parseChord("ctrl+wheeldown") ==
           MouseChord{MouseInput::WheelDown, true}));
    CHECK((Mousemap::parseChord(" Shift+Alt+Middle ") ==
           MouseChord{MouseInput::Middle, false, true, true}));
    CHECK((Mousemap::parseChord("double_click") == MouseChord{MouseInput::DoubleClick}));
    CHECK(!Mousemap::parseChord("Left"));   // 左右ボタンは割り当ての対象外
    CHECK(!Mousemap::parseChord("Wheel"));  // 向きの無い表記は受け付けない
    CHECK(!Mousemap::parseChord("Ctrl+"));
    CHECK(!Mousemap::parseChord(""));
    CHECK(Mousemap::chordToString({MouseInput::None}).empty());
    for (const MouseInput input :
         {MouseInput::Middle, MouseInput::X1, MouseInput::X2, MouseInput::DoubleClick,
          MouseInput::WheelUp, MouseInput::WheelDown, MouseInput::WheelLeft,
          MouseInput::WheelRight}) {
        for (const bool ctrl : {false, true}) {
            const MouseChord chord{input, ctrl};
            const std::string text = Mousemap::chordToString(chord);
            CHECK(!text.empty());
            CHECK((Mousemap::parseChord(text) == chord));
            CHECK(!Mousemap::chordToDisplayString(chord).empty());
        }
    }

    // chordsFor の並びは修飾なしが先で、格納順(unordered_map)に依存しない
    const std::vector<MouseChord> next = mm.chordsFor(Command::NextImage);
    CHECK(next.size() == 3);
    CHECK((next[0] == MouseChord{MouseInput::X2}));
    CHECK((next[1] == MouseChord{MouseInput::WheelRight}));
    CHECK((next[2] == MouseChord{MouseInput::WheelDown, true}));
    CHECK(mm.chordsFor(Command::ZoomIn).empty());

    // ini はコマンドごとに既存の割り当てを置き換える。コマンド名でないキー
    // (swap_buttons)は無視する
    Mousemap custom = Mousemap::defaults();
    custom.applyConfig({{"next", "WheelDown, X2"},
                        {"prev", "WheelUp"},
                        {"fullscreen", "DoubleClick"},
                        {"swap_buttons", "true"},
                        {"unknown_command", "X1"}});
    CHECK(custom.find({MouseInput::WheelDown}) == Command::NextImage);
    CHECK(custom.find({MouseInput::X2}) == Command::NextImage);
    CHECK(custom.find({MouseInput::WheelDown, true}) == Command::None);  // 置き換えられた
    CHECK(custom.find({MouseInput::WheelRight}) == Command::None);
    CHECK(custom.find({MouseInput::X1}) == Command::None);  // prev は WheelUp だけになった
    CHECK(custom.find({MouseInput::WheelUp}) == Command::PrevImage);
    CHECK(custom.find({MouseInput::DoubleClick}) == Command::ToggleFullscreen);

    // 解析できない表記が混ざっていても、残りは割り当てられる
    Mousemap partial = Mousemap::defaults();
    partial.applyConfig({{"last", "Nonsense, X1"}});
    CHECK(partial.find({MouseInput::X1}) == Command::LastImage);

    // ホイールの蓄積: 1 ノッチに達するまでは何も起きず、達した分だけ段が出る
    float accum = 0.0f;
    CHECK(consumeWheelSteps(accum, 0.4f) == 0);
    CHECK(consumeWheelSteps(accum, 0.4f) == 0);
    CHECK(consumeWheelSteps(accum, 0.4f) == 1);  // 1.2 → 1 段(0.2 は残す)
    CHECK(consumeWheelSteps(accum, 1.0f) == 1);
    CHECK(consumeWheelSteps(accum, 3.0f) == 3);  // 一度に何段でも出る
    CHECK(consumeWheelSteps(accum, 0.0f) == 0);
    // 逆向きに回したら貯金は捨てる(小さく戻したときに 1 回目が飲まれない)
    accum = 0.0f;
    CHECK(consumeWheelSteps(accum, 0.9f) == 0);
    CHECK(consumeWheelSteps(accum, -0.9f) == 0);
    CHECK(consumeWheelSteps(accum, -0.9f) == -1);

    // しきい値を上げると、その分だけ回さないと 1 段にならない(水平ホイールの誤爆対策)
    accum = 0.0f;
    CHECK(consumeWheelSteps(accum, 1.0f, 2.0f) == 0);
    CHECK(consumeWheelSteps(accum, 0.9f, 2.0f) == 0);
    CHECK(consumeWheelSteps(accum, 0.2f, 2.0f) == 1);  // 2.1 → 1 段(0.1 は残す)
    CHECK(consumeWheelSteps(accum, 4.0f, 2.0f) == 2);  // 4.1 → 2 段(0.1 は残す)
    CHECK(consumeWheelSteps(accum, -4.0f, 2.0f) == -2);
    // 0 以下のしきい値は 1.0 として扱う(0 除算を避ける)
    accum = 0.0f;
    CHECK(consumeWheelSteps(accum, 1.0f, 0.0f) == 1);
}

void testNavArrows() {
    const SizeF viewport{800, 600};
    // 800x600 なら 左ボタン x 12-56 / 右ボタン x 744-788、y 278-322(上下中央)、帯は 110px
    const auto state = [&viewport](std::optional<Point> pointer, bool hasPrev = true,
                                   bool hasNext = true) {
        return navArrowsState(viewport, pointer, hasPrev, hasNext);
    };

    // ポインタが無い(ウィンドウ外・ドラッグ中)なら出さない
    CHECK(!state(std::nullopt).prev.visible);
    CHECK(!state(std::nullopt).next.visible);
    // 中央では出さない(端の帯に入ったときだけ)
    CHECK(!state(Point{400, 300}).prev.visible);
    CHECK(!state(Point{400, 300}).next.visible);
    // 左の帯 → 左だけ、右の帯 → 右だけ
    CHECK(state(Point{90, 300}).prev.visible);
    CHECK(!state(Point{90, 300}).next.visible);
    CHECK(state(Point{700, 300}).next.visible);
    CHECK(!state(Point{700, 300}).prev.visible);
    // 帯の中でもボタンの上でなければホバーしない
    CHECK(!state(Point{90, 300}).prev.hovered);
    CHECK(state(Point{30, 300}).prev.hovered);
    CHECK(state(Point{760, 300}).next.hovered);
    // ボタンは上下中央、端から kNavArrowMarginPx
    const NavArrow prev = state(Point{30, 300}).prev;
    CHECK(prev.p1.x == kNavArrowMarginPx);
    CHECK(prev.p2.x == kNavArrowMarginPx + kNavArrowSizePx);
    CHECK(prev.p1.y == (600 - kNavArrowSizePx) / 2);
    CHECK(prev.p2.y == prev.p1.y + kNavArrowSizePx);
    const NavArrow next = state(Point{760, 300}).next;
    CHECK(next.p2.x == 800 - kNavArrowMarginPx);
    CHECK(next.p1.x == next.p2.x - kNavArrowSizePx);
    // 先頭 / 末尾では行き先が無いほうを出さない
    CHECK(!state(Point{30, 300}, false, true).prev.visible);
    CHECK(!state(Point{760, 300}, true, false).next.visible);
    // ポインタがビューポートの外(サイドバー・ステータスバー上)なら出さない
    CHECK(!state(Point{-10, 300}).prev.visible);
    CHECK(!state(Point{30, 700}).prev.visible);
    // ボタンが収まらない狭いビューポートでは出さない
    CHECK(!navArrowsState({150, 600}, Point{10, 300}, true, true).prev.visible);
    CHECK(!navArrowsState({800, 60}, Point{30, 30}, true, true).prev.visible);

    // クリック判定はボタンの内側だけ(帯全体を当たりにしない)
    const NavArrowsState both = state(Point{30, 300});
    CHECK(hitTestNavArrows(both, Point{30, 300}) == std::optional<bool>{false});
    CHECK(!hitTestNavArrows(both, Point{90, 300}));  // 帯の中だが枠外
    const NavArrowsState nextShown = state(Point{760, 300});
    CHECK(hitTestNavArrows(nextShown, Point{760, 300}) == std::optional<bool>{true});
    // 出ていないボタンには当たらない
    CHECK(!hitTestNavArrows(state(Point{30, 300}, false, true), Point{30, 300}));
}

void testHelpLines() {
    const Keymap km = Keymap::defaults();
    const Mousemap mm = Mousemap::defaults();
    CHECK(keysLabel(km, Command::NextImage) == "Right Down PageDown");
    CHECK(keysLabel(km, Command::ToggleHelp) == "F1");
    CHECK(keysLabel(km, Command::SelectToolArrow).empty());  // 既定では未割り当て
    CHECK(mouseLabel(mm, Command::NextImage) == "サイド(進む) チルト→ Ctrl+ホイール↓");
    CHECK(mouseLabel(mm, Command::ZoomIn).empty());  // ズームは割り当てではない

    const std::vector<HelpLine> lines = buildHelpLines(km, mm, false);
    const auto has = [&lines](std::string_view text) {
        return std::any_of(lines.begin(), lines.end(),
                           [text](const HelpLine& line) { return line.text == text; });
    };
    const auto hasHeader = [&lines](std::string_view text) {
        return std::any_of(lines.begin(), lines.end(), [text](const HelpLine& line) {
            return line.header && line.text == text;
        });
    };

    CHECK(hasHeader("表示"));
    CHECK(hasHeader("マウス"));
    CHECK(has("次の画像  Right Down PageDown")
          && has("アニメーション再生 / 一時停止  Space")
          && has("次のフレーム / ページ  Shift+Right"));
    CHECK(has("この操作一覧  F1"));
    CHECK(has("右 90 度回転  R"));
    CHECK(has("パスをコピー  Ctrl+Shift+C"));
    CHECK(has("上書き保存  Ctrl+S"));
    CHECK(has("名前を付けて保存  Ctrl+Shift+S"));
    CHECK(has("印刷  Ctrl+P"));
    CHECK(has("ファイルをコピー  Shift+C"));
    // キーの割り当てがない操作は行ごと出ない
    CHECK(!has("矢印ツール  "));
    CHECK(std::none_of(lines.begin(), lines.end(), [](const HelpLine& line) {
        return line.text.starts_with("矢印ツール");
    }));

    // マウス操作の行は実際に効くボタンを出す(メニューは入れ替えないので右のまま)
    CHECK(has("スクロール  左ドラッグ"));
    CHECK(has("現在のツールを実行  右ドラッグ"));
    CHECK(has("ツール・書式メニュー  余白で右クリック"));
    // マウスの割り当ては Mousemap から生成する
    CHECK(has("次の画像  サイド(進む) チルト→ Ctrl+ホイール↓"));
    CHECK(has("前の画像  サイド(戻る) チルト← Ctrl+ホイール↑"));
    // 素のホイールが空いている限りズームは「ホイール」
    CHECK(has("拡大 / 縮小  ホイール"));
    const std::vector<HelpLine> swapped = buildHelpLines(km, mm, true);
    const auto hasSwapped = [&swapped](std::string_view text) {
        return std::any_of(swapped.begin(), swapped.end(),
                           [text](const HelpLine& line) { return line.text == text; });
    };
    CHECK(hasSwapped("スクロール  右ドラッグ"));
    CHECK(hasSwapped("現在のツールを実行  左ドラッグ"));
    CHECK(hasSwapped("正方形 / 真円で描く  Shift+左ドラッグ"));
    CHECK(hasSwapped("直線・矢印・手書きをまっすぐ  Shift+左ドラッグ"));
    // オブジェクトを掴む操作は入れ替えの対象外なので、入れ替えても左のまま
    CHECK(hasSwapped("図形・テキストを選択  左クリック"));
    CHECK(hasSwapped("選択中のオブジェクトを移動  左ドラッグ"));
    CHECK(hasSwapped("ツール・書式メニュー  余白で右クリック"));

    // ini でキーを変えたら一覧もそれに追従する(README のような固定テキストではない)
    Keymap custom = Keymap::defaults();
    custom.applyConfig({{"next", "N"}, {"tool_arrow", "A"}});
    Mousemap customMouse = Mousemap::defaults();
    customMouse.applyConfig({{"next", "WheelDown"}, {"prev", "WheelUp"}, {"fit", "Middle"}});
    const std::vector<HelpLine> customLines = buildHelpLines(custom, customMouse, false);
    const auto hasCustom = [&customLines](std::string_view text) {
        return std::any_of(customLines.begin(), customLines.end(),
                           [text](const HelpLine& line) { return line.text == text; });
    };
    CHECK(hasCustom("次の画像  N"));
    CHECK(hasCustom("矢印ツール  A"));
    // ini でマウスを変えたら一覧もそれに追従する。素のホイールを遷移で埋めたので
    // ズームの案内は空いている Ctrl+ホイールへ移る
    CHECK(hasCustom("次の画像  ホイール↓"));
    CHECK(hasCustom("ウィンドウにフィット  中ボタン"));
    CHECK(hasCustom("拡大 / 縮小  Ctrl+ホイール"));
    CHECK(!hasCustom("拡大 / 縮小  ホイール"));

    // 節の中身が全部消えたら見出しも出さない
    Keymap stripped = Keymap::defaults();
    stripped.unbindCommand(Command::OpenFile);
    stripped.unbindCommand(Command::SaveImage);
    stripped.unbindCommand(Command::SaveImageAs);
    stripped.unbindCommand(Command::PrintImage);
    stripped.unbindCommand(Command::CopyImage);
    stripped.unbindCommand(Command::CopyPath);
    stripped.unbindCommand(Command::CopyFile);
    stripped.unbindCommand(Command::CopyOcrText);
    stripped.unbindCommand(Command::PasteImage);
    stripped.unbindCommand(Command::PasteObject);
    const std::vector<HelpLine> strippedLines = buildHelpLines(stripped, mm, false);
    CHECK(std::none_of(strippedLines.begin(), strippedLines.end(),
                       [](const HelpLine& line) { return line.text == "ファイル"; }));
}

void testConfig() {
    const Config cfg = Config::parse(
        "# コメント\n"
        "[View]\n"
        "Background = #FF8000\n"
        "fit_upscale = true\n"
        "prefetch_radius = 3\n"
        "; これもコメント\n"
        "[keys]\n"
        "next = N\n"
        "broken_line_without_equal\n");
    CHECK(cfg.getColorRGB("view", "background", 0) == 0xFF8000u);
    CHECK(cfg.getBool("view", "fit_upscale", false) == true);
    CHECK(cfg.getInt("view", "prefetch_radius", 2) == 3);
    CHECK(cfg.get("keys", "next") == "N");
    CHECK(cfg.get("keys", "missing", "def") == "def");
    CHECK(cfg.getInt("view", "missing", 42) == 42);
    CHECK(cfg.getColorRGB("view", "fit_upscale", 7) == 7u);  // 色として不正 → 既定値
    // UTF-8 BOM 付きでも先頭セクションが読める。剥がさないと "\xEF\xBB\xBF[view]" が
    // '[' で始まらず、最初のセクションだけ黙って無視される
    const Config bom = Config::parse("\xEF\xBB\xBF[view]\nfit_upscale = true\n[keys]\nnext = N\n");
    CHECK(bom.getBool("view", "fit_upscale", false) == true);
    CHECK(bom.get("keys", "next") == "N");

}

// 3x2 のテスト画像。各画素の R に連番 (10,11,12 / 20,21,22) を入れて位置を追跡する
blinker::DecodedImage makeOrientImage() {
    blinker::DecodedImage img;
    img.width = 3;
    img.height = 2;
    img.pixels.resize(3 * 2 * 4);
    for (uint32_t y = 0; y < 2; ++y) {
        for (uint32_t x = 0; x < 3; ++x) {
            uint8_t* p = img.pixels.data() + (y * 3 + x) * 4;
            p[0] = 0;
            p[1] = 0;
            p[2] = static_cast<uint8_t>((y + 1) * 10 + x);  // R に「行,列」を符号化
            p[3] = 255;
        }
    }
    return img;
}

// 画像の R 成分を左上から行優先で並べた列にする
std::vector<uint8_t> redsOf(const blinker::DecodedImage& img) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i < static_cast<size_t>(img.width) * img.height; ++i) {
        out.push_back(img.pixels[i * 4 + 2]);
    }
    return out;
}

void testApplyExifOrientation() {
    using blinker::applyExifOrientation;
    // 元画像 (3x2):  10 11 12
    //                20 21 22
    const std::vector<uint8_t> original = {10, 11, 12, 20, 21, 22};
    CHECK(redsOf(makeOrientImage()) == original);

    // 1 と範囲外は何もしない
    for (const uint16_t o : {uint16_t{0}, uint16_t{1}, uint16_t{9}}) {
        auto img = makeOrientImage();
        CHECK(!applyExifOrientation(img, o));
        CHECK(img.width == 3 && img.height == 2);
        CHECK(redsOf(img) == original);
    }

    // 2: 左右反転 → 12 11 10 / 22 21 20
    auto img2 = makeOrientImage();
    CHECK(applyExifOrientation(img2, 2));
    CHECK(img2.width == 3 && img2.height == 2);
    CHECK(redsOf(img2) == std::vector<uint8_t>({12, 11, 10, 22, 21, 20}));

    // 3: 180 度 → 22 21 20 / 12 11 10
    auto img3 = makeOrientImage();
    CHECK(applyExifOrientation(img3, 3));
    CHECK(img3.width == 3 && img3.height == 2);
    CHECK(redsOf(img3) == std::vector<uint8_t>({22, 21, 20, 12, 11, 10}));

    // 4: 上下反転 → 20 21 22 / 10 11 12
    auto img4 = makeOrientImage();
    CHECK(applyExifOrientation(img4, 4));
    CHECK(redsOf(img4) == std::vector<uint8_t>({20, 21, 22, 10, 11, 12}));

    // 5〜8 は縦横が入れ替わり 2x3 になる
    // 5: 主対角で転置 → 10 20 / 11 21 / 12 22
    auto img5 = makeOrientImage();
    CHECK(applyExifOrientation(img5, 5));
    CHECK(img5.width == 2 && img5.height == 3);
    CHECK(redsOf(img5) == std::vector<uint8_t>({10, 20, 11, 21, 12, 22}));

    // 6: 時計回り 90 度 → 左下が左上へ来る → 20 10 / 21 11 / 22 12
    auto img6 = makeOrientImage();
    CHECK(applyExifOrientation(img6, 6));
    CHECK(img6.width == 2 && img6.height == 3);
    CHECK(redsOf(img6) == std::vector<uint8_t>({20, 10, 21, 11, 22, 12}));

    // 7: 副対角で転置 → 22 12 / 21 11 / 20 10
    auto img7 = makeOrientImage();
    CHECK(applyExifOrientation(img7, 7));
    CHECK(redsOf(img7) == std::vector<uint8_t>({22, 12, 21, 11, 20, 10}));

    // 8: 時計回り 270 度 → 12 22 / 11 21 / 10 20
    auto img8 = makeOrientImage();
    CHECK(applyExifOrientation(img8, 8));
    CHECK(img8.width == 2 && img8.height == 3);
    CHECK(redsOf(img8) == std::vector<uint8_t>({12, 22, 11, 21, 10, 20}));

    // 90 度を 4 回で元に戻る(回転の整合性)
    auto round = makeOrientImage();
    for (int i = 0; i < 4; ++i) CHECK(applyExifOrientation(round, 6));
    CHECK(round.width == 3 && round.height == 2);
    CHECK(redsOf(round) == original);

    // 反転・転置は 2 回で元に戻る
    for (const uint16_t o : {uint16_t{2}, uint16_t{3}, uint16_t{4}, uint16_t{5}, uint16_t{7}}) {
        auto img = makeOrientImage();
        CHECK(applyExifOrientation(img, o));
        CHECK(applyExifOrientation(img, o));
        CHECK(img.width == 3 && img.height == 2);
        CHECK(redsOf(img) == original);
    }

    // 壊れた入力(ピクセル数が足りない)では何もしない
    blinker::DecodedImage broken;
    broken.width = 100;
    broken.height = 100;
    broken.pixels.resize(16);
    CHECK(!applyExifOrientation(broken, 6));
    CHECK(broken.width == 100 && broken.height == 100);

    blinker::DecodedImage empty;
    CHECK(!applyExifOrientation(empty, 6));
}

// Orientation 1 件だけを持つ最小の TIFF (Exif 本体) を組み立てる
std::vector<uint8_t> makeExifTiff(uint16_t orientation, bool bigEndian) {
    const auto lo = static_cast<uint8_t>(orientation & 0xFF);
    const auto hi = static_cast<uint8_t>(orientation >> 8);
    if (bigEndian) {
        return {'M',  'M',  0x00, 0x2A, 0x00, 0x00, 0x00, 0x08,  // ヘッダ (IFD0 は +8)
                0x00, 0x01,                                     // エントリ数
                0x01, 0x12, 0x00, 0x03,                         // Orientation / SHORT
                0x00, 0x00, 0x00, 0x01,                         // 個数
                hi,   lo,   0x00, 0x00};  // 値(4 バイト枠の先頭に詰める)
    }
    return {'I',  'I',  0x2A, 0x00, 0x08, 0x00, 0x00, 0x00,
            0x01, 0x00,
            0x12, 0x01, 0x03, 0x00,
            0x01, 0x00, 0x00, 0x00,
            lo,   hi,   0x00, 0x00};
}

// APP1 (Exif) セグメントだけを持つ JPEG のバイト列(画像データは無い)
std::vector<uint8_t> makeExifJpeg(const std::vector<uint8_t>& tiff) {
    std::vector<uint8_t> out = {0xFF, 0xD8};
    const size_t length = 2 + 6 + tiff.size();  // 長さフィールド + "Exif\0\0" + TIFF
    out.push_back(0xFF);
    out.push_back(0xE1);
    out.push_back(static_cast<uint8_t>(length >> 8));  // 長さはビッグエンディアン
    out.push_back(static_cast<uint8_t>(length & 0xFF));
    for (const char c : std::string_view("Exif\0\0", 6)) out.push_back(static_cast<uint8_t>(c));
    out.insert(out.end(), tiff.begin(), tiff.end());
    out.push_back(0xFF);
    out.push_back(0xD9);  // EOI
    return out;
}

// eXIf チャンクを持つ PNG のバイト列(IHDR/IDAT は無い)
std::vector<uint8_t> makeExifPng(const std::vector<uint8_t>& tiff) {
    std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    const auto size = static_cast<uint32_t>(tiff.size());
    out.push_back(static_cast<uint8_t>(size >> 24));
    out.push_back(static_cast<uint8_t>(size >> 16));
    out.push_back(static_cast<uint8_t>(size >> 8));
    out.push_back(static_cast<uint8_t>(size));
    for (const char c : std::string_view("eXIf")) out.push_back(static_cast<uint8_t>(c));
    out.insert(out.end(), tiff.begin(), tiff.end());
    out.insert(out.end(), 4, 0x00);  // CRC(読み飛ばされるので中身は問わない)
    return out;
}

void testReadExifOrientation() {
    using blinker::readExifOrientation;
    const auto read = [](const std::vector<uint8_t>& bytes) {
        return readExifOrientation(bytes.data(), bytes.size());
    };

    // JPEG の APP1 / PNG の eXIf、リトルエンディアン・ビッグエンディアンとも読める
    for (const bool bigEndian : {false, true}) {
        for (const uint16_t o : {uint16_t{1}, uint16_t{3}, uint16_t{6}, uint16_t{8}}) {
            const std::vector<uint8_t> tiff = makeExifTiff(o, bigEndian);
            CHECK(read(makeExifJpeg(tiff)) == o);
            CHECK(read(makeExifPng(tiff)) == o);
        }
    }

    // 範囲外の値は 1(回転なし)へ丸める
    CHECK(read(makeExifJpeg(makeExifTiff(0, false))) == 1);
    CHECK(read(makeExifJpeg(makeExifTiff(9, false))) == 1);

    // Exif の無い JPEG(別の APP セグメントは読み飛ばして SOS で止まる)
    std::vector<uint8_t> plain = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x04, 0x00, 0x00,
                                  0xFF, 0xDA, 0x00, 0x02};
    CHECK(read(plain) == 1);

    // 途中で切れた Exif、TIFF のマジックが違う、他形式、空
    std::vector<uint8_t> truncated = makeExifJpeg(makeExifTiff(6, false));
    truncated.resize(truncated.size() - 8);
    CHECK(read(truncated) == 1);
    std::vector<uint8_t> badMagic = makeExifTiff(6, false);
    badMagic[0] = 'X';
    CHECK(read(makeExifJpeg(badMagic)) == 1);
    const std::vector<uint8_t> bmp = {'B', 'M', 0x36, 0, 0, 0, 0, 0, 0, 0, 0x36, 0, 0, 0};
    CHECK(read(bmp) == 1);
    CHECK(readExifOrientation(nullptr, 0) == 1);
    CHECK(read({0xFF, 0xD8}) == 1);
}

void testImageList() {
    ImageList list;
    CHECK(list.empty());

    list.set({"C:/pics/1.png", "C:/pics/2.png", "C:/pics/10.png"}, "C:/PICS/2.PNG");
    CHECK(list.size() == 3);
    CHECK(list.index() == 1);  // 大文字小文字を無視して一致

    CHECK(list.next());
    CHECK(list.index() == 2);
    CHECK(!list.next());  // 末尾で停止
    CHECK(list.first());
    CHECK(list.index() == 0);
    CHECK(!list.prev());  // 先頭で停止
    CHECK(list.last());
    CHECK(list.index() == 2);

    list.set({"a.png", "b.png", "c.png", "d.png", "e.png"}, "c.png");
    CHECK(list.at(3).filename() == "d.png");
    CHECK(list.jumpTo(4));
    CHECK(list.index() == 4);
    CHECK(!list.jumpTo(4));  // 同じ位置は false
    CHECK(!list.jumpTo(5));  // 範囲外は無視
    CHECK(list.index() == 4);
    CHECK(list.jumpTo(2));

    const auto order = list.prefetchOrder(2);
    CHECK(order.size() == 4);
    CHECK(order[0].filename() == "d.png");  // +1 が最優先
    CHECK(order[1].filename() == "b.png");
    CHECK(order[2].filename() == "e.png");
    CHECK(order[3].filename() == "a.png");
}

// ---- DIB 変換 (クリップボード貼り付け用) ----

void putU16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x));
    v.push_back(static_cast<uint8_t>(x >> 8));
}

void putU32(std::vector<uint8_t>& v, uint32_t x) {
    putU16(v, static_cast<uint16_t>(x));
    putU16(v, static_cast<uint16_t>(x >> 16));
}

// 40 バイトの BITMAPINFOHEADER を組み立てる
std::vector<uint8_t> dibHeader(int32_t width, int32_t height, uint16_t bitCount,
                               uint32_t compression, uint32_t clrUsed = 0) {
    std::vector<uint8_t> v;
    putU32(v, 40);
    putU32(v, static_cast<uint32_t>(width));
    putU32(v, static_cast<uint32_t>(height));
    putU16(v, 1);  // planes
    putU16(v, bitCount);
    putU32(v, compression);
    putU32(v, 0);  // sizeImage
    putU32(v, 0);  // xppm
    putU32(v, 0);  // yppm
    putU32(v, clrUsed);
    putU32(v, 0);  // clrImportant
    return v;
}

// 124 バイトの BITMAPV5HEADER (BI_BITFIELDS、BGRA マスク付き)
std::vector<uint8_t> dibV5Header(int32_t width, int32_t height) {
    std::vector<uint8_t> v = dibHeader(width, height, 32, 3 /*BI_BITFIELDS*/);
    v[0] = 124;  // bV5Size
    putU32(v, 0x00FF0000);  // red
    putU32(v, 0x0000FF00);  // green
    putU32(v, 0x000000FF);  // blue
    putU32(v, 0xFF000000);  // alpha
    v.resize(124, 0);
    return v;
}

// 出力 (PBGRA) の (x, y) が期待値どおりか
bool pixelIs(const DecodedImage& img, uint32_t x, uint32_t y, uint8_t b, uint8_t g, uint8_t r,
             uint8_t a) {
    const uint8_t* p = img.pixels.data() + (static_cast<size_t>(y) * img.width + x) * 4;
    return p[0] == b && p[1] == g && p[2] == r && p[3] == a;
}

void testDib() {
    // 32bpp BI_RGB 2x2 ボトムアップ。第4バイトは未定義なので不透明扱い
    {
        auto d = dibHeader(2, 2, 32, 0);
        // 格納順は下の行から: (0,1)=青, (1,1)=白 / (0,0)=赤, (1,0)=緑(第4バイトにゴミ)
        putU32(d, 0x000000FF);  // 青 (XXRRGGBB リトルエンディアン格納 → B,G,R,X)
        putU32(d, 0x00FFFFFF);  // 白
        putU32(d, 0x00FF0000);  // 赤
        putU32(d, 0x7F00FF00);  // 緑 + ゴミアルファ 0x7F
        const auto img = imageFromDib(d.data(), d.size());
        CHECK(img && img->width == 2 && img->height == 2);
        CHECK(pixelIs(*img, 0, 0, 0, 0, 255, 255));      // 赤
        CHECK(pixelIs(*img, 1, 0, 0, 255, 0, 255));      // 緑 (ゴミアルファは無視)
        CHECK(pixelIs(*img, 0, 1, 255, 0, 0, 255));      // 青
        CHECK(pixelIs(*img, 1, 1, 255, 255, 255, 255));  // 白

        // トップダウン (高さ負) は格納順のまま
        auto t = dibHeader(2, -2, 32, 0);
        putU32(t, 0x000000FF);
        putU32(t, 0x00FFFFFF);
        putU32(t, 0x00FF0000);
        putU32(t, 0x7F00FF00);
        const auto timg = imageFromDib(t.data(), t.size());
        CHECK(timg && pixelIs(*timg, 0, 0, 255, 0, 0, 255));  // 先頭行が上 → (0,0)=青
        CHECK(pixelIs(*timg, 1, 1, 0, 255, 0, 255));
    }

    // CF_DIBV5: アルファマスク付き。ストレート → 事前乗算される
    {
        auto d = dibV5Header(1, 1);
        putU32(d, 0x800000FF);  // 青、アルファ 128
        const auto img = imageFromDib(d.data(), d.size());
        CHECK(img && pixelIs(*img, 0, 0, 128, 0, 0, 128));  // (255*128+127)/255 = 128
    }

    // アルファマスク付きなのに全ピクセル a=0 → 不透明として救済
    {
        auto d = dibV5Header(2, 1);
        putU32(d, 0x001E140A);  // (B,G,R) = (10,20,30), a=0
        putU32(d, 0x00000000);
        const auto img = imageFromDib(d.data(), d.size());
        CHECK(img && pixelIs(*img, 0, 0, 10, 20, 30, 255));
        CHECK(pixelIs(*img, 1, 0, 0, 0, 0, 255));
    }

    // 24bpp: 行が 4 バイト境界にパディングされる (幅3 → 9 バイト + 3 パディング)
    {
        auto d = dibHeader(3, 2, 24, 0);
        const uint8_t rows[2][12] = {
            {1, 2, 3, 4, 5, 6, 7, 8, 9, 0, 0, 0},           // 下の行
            {10, 20, 30, 40, 50, 60, 70, 80, 90, 0, 0, 0},  // 上の行
        };
        for (const auto& row : rows) d.insert(d.end(), row, row + 12);
        const auto img = imageFromDib(d.data(), d.size());
        CHECK(img && img->width == 3 && img->height == 2);
        CHECK(pixelIs(*img, 0, 0, 10, 20, 30, 255));
        CHECK(pixelIs(*img, 2, 0, 70, 80, 90, 255));
        CHECK(pixelIs(*img, 2, 1, 7, 8, 9, 255));
    }

    // 16bpp BI_BITFIELDS (565): マスク 3 個が 40 バイトヘッダの直後に続く
    {
        auto d = dibHeader(2, 1, 16, 3);
        putU32(d, 0xF800);  // red
        putU32(d, 0x07E0);  // green
        putU32(d, 0x001F);  // blue
        putU16(d, 0xF800);  // 赤ピクセル
        putU16(d, 0x07E0);  // 緑ピクセル
        const auto img = imageFromDib(d.data(), d.size());
        CHECK(img && pixelIs(*img, 0, 0, 0, 0, 255, 255));
        CHECK(pixelIs(*img, 1, 0, 0, 255, 0, 255));
    }

    // 16bpp BI_RGB は 555 固定
    {
        auto d = dibHeader(1, 1, 16, 0);
        putU16(d, 0x7C00);  // 赤 (5bit 最大値 → 255 にスケール)
        putU16(d, 0);       // パディング
        const auto img = imageFromDib(d.data(), d.size());
        CHECK(img && pixelIs(*img, 0, 0, 0, 0, 255, 255));
    }

    // 8bpp パレット
    {
        auto d = dibHeader(2, 1, 8, 0, 2);
        putU32(d, 0x00030201);  // パレット0: B=1, G=2, R=3
        putU32(d, 0x0096C8FA);  // パレット1: B=250, G=200, R=150
        d.push_back(1);         // (0,0) → パレット1
        d.push_back(0);         // (1,0) → パレット0
        putU16(d, 0);           // 行パディング
        const auto img = imageFromDib(d.data(), d.size());
        CHECK(img && pixelIs(*img, 0, 0, 250, 200, 150, 255));
        CHECK(pixelIs(*img, 1, 0, 1, 2, 3, 255));
    }

    // 不正データは nullptr
    {
        auto d = dibHeader(2, 2, 32, 0);
        for (int i = 0; i < 4; ++i) putU32(d, 0);
        CHECK(imageFromDib(d.data(), d.size() - 1) == nullptr);  // ピクセル不足
        CHECK(imageFromDib(d.data(), 39) == nullptr);            // ヘッダ不足
        CHECK(imageFromDib(nullptr, 100) == nullptr);

        const auto reject = [](std::vector<uint8_t> header) {
            header.resize(4096, 0);  // ピクセル領域は十分に確保した上でヘッダだけ不正にする
            return imageFromDib(header.data(), header.size()) == nullptr;
        };
        CHECK(reject(dibHeader(2, 2, 4, 0)));        // 4bpp 非対応
        CHECK(reject(dibHeader(2, 2, 8, 1)));        // RLE 非対応
        CHECK(reject(dibHeader(0, 2, 32, 0)));       // 幅 0
        CHECK(reject(dibHeader(100000, 1, 32, 0)));  // 巨大
    }
}

void testPixelConvert() {
    DecodedImage img;
    img.width = 3;
    img.height = 1;
    // 事前乗算 (128,64,32, a=128) / 不透明 (10,20,30) / 完全透明
    img.pixels = {128, 64, 32, 128, 10, 20, 30, 255, 0, 0, 0, 0};

    const auto straight = toStraightBGRA(img);
    CHECK(straight.size() == 12);
    CHECK(straight[0] == 255 && straight[1] == 128 && straight[2] == 64 && straight[3] == 128);
    CHECK(straight[4] == 10 && straight[5] == 20 && straight[6] == 30 && straight[7] == 255);
    CHECK(straight[8] == 0 && straight[11] == 0);

    const auto opaque = toOpaqueBGR(img);
    CHECK(opaque.size() == 9);
    // 白合成: premult + (255 - a)
    CHECK(opaque[0] == 255 && opaque[1] == 191 && opaque[2] == 159);
    CHECK(opaque[3] == 10 && opaque[4] == 20 && opaque[5] == 30);
    CHECK(opaque[6] == 255 && opaque[7] == 255 && opaque[8] == 255);  // 透明 → 白
}

// 1x1 のダミー画像を返すテスト用デコーダ。"fail" を含むパスは失敗させる
class FakeDecoder final : public IImageDecoder {
public:
    std::shared_ptr<DecodedImage> decode(const std::filesystem::path& path,
                                         std::string* error = nullptr) override {
        if (pathToUtf8(path).find("fail") != std::string::npos) {
            if (error) *error = "ピクセル取得 (0x88982F50)";
            return nullptr;
        }
        auto image = std::make_shared<DecodedImage>();
        image->width = 1;
        image->height = 1;
        image->pixels = {0, 0, 0, 255};
        return image;
    }
};

// デコードに時間がかかるデコーダ。openPath の同期取得に間に合わないケースを作る
class SlowDecoder final : public IImageDecoder {
public:
    std::shared_ptr<DecodedImage> decode(const std::filesystem::path&,
                                         std::string* = nullptr) override {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        auto image = std::make_shared<DecodedImage>();
        image->width = 1;
        image->height = 1;
        image->pixels = {0, 0, 0, 255};
        return image;
    }
};

void testImageCache() {
    FakeDecoder decoder;
    ImageCache cache(decoder);

    std::mutex mutex;
    std::condition_variable cv;
    int decodedCount = 0;
    cache.setOnDecoded([&](const std::filesystem::path&) {
        std::lock_guard lock(mutex);
        ++decodedCount;
        cv.notify_all();
    });

    cache.requestNow("ok.png");
    cache.requestNow("fail.png");
    {
        std::unique_lock lock(mutex);
        const bool done = cv.wait_for(lock, std::chrono::seconds(5),
                                      [&] { return decodedCount >= 2; });
        CHECK(done);
    }

    bool failed = false;
    std::string error = "残っていてはいけない値";
    const auto image = cache.tryGet("ok.png", &failed, &error);
    CHECK(image != nullptr);
    CHECK(!failed);
    CHECK(image->width == 1);
    CHECK(error.empty());  // 成功時は理由を残さない

    const auto none = cache.tryGet("fail.png", &failed, &error);
    CHECK(none == nullptr);
    CHECK(failed);
    CHECK(error == "ピクセル取得 (0x88982F50)");  // デコーダの失敗理由がそのまま届く

    // 未デコードのパスでは失敗理由も付かない
    CHECK(cache.tryGet("never_requested.png", &failed, &error) == nullptr);
    CHECK(!failed);
    CHECK(error.empty());

    // invalidate: 1 件だけ捨てる(上書き保存でファイルが変わったとき)
    cache.invalidate("ok.png");
    CHECK(cache.tryGet("ok.png", &failed, &error) == nullptr);
    CHECK(!failed);  // 失敗ではなく「未デコード」に戻る
    CHECK(cache.tryGet("fail.png", &failed, &error) == nullptr && failed);  // 他は残る
    cache.invalidate("never_requested.png");  // 無いパスでも安全
}

// 1 枚 4MB (1024x1024) の画像を返すデコーダ。バイト上限による破棄を確かめる
class BigDecoder final : public IImageDecoder {
public:
    std::shared_ptr<DecodedImage> decode(const std::filesystem::path&,
                                         std::string* = nullptr) override {
        auto image = std::make_shared<DecodedImage>();
        image->width = 1024;
        image->height = 1024;
        image->pixels.assign(size_t{1024} * 1024 * 4, 0);
        return image;
    }
};

void testImageCacheLimits() {
    constexpr size_t kMB = size_t{1} << 20;

    // [cache] が無ければ既定値
    const ImageCacheLimits defaults = cacheLimitsFromConfig(Config::parse(""));
    CHECK(defaults.maxBytes == 512 * kMB);
    CHECK(defaults.maxItems == 8);

    const auto limits = cacheLimitsFromConfig(
        Config::parse("[cache]\nmax_memory_mb = 128\nmax_items = 4\n"));
    CHECK(limits.maxBytes == 128 * kMB);
    CHECK(limits.maxItems == 4);

    // 範囲外は丸める(小さすぎる指定で 1 枚も持てなくなるのを防ぐ)
    const auto low =
        cacheLimitsFromConfig(Config::parse("[cache]\nmax_memory_mb = 0\nmax_items = 1\n"));
    CHECK(low.maxBytes == static_cast<size_t>(kMinCacheMemoryMB) * kMB);
    CHECK(low.maxItems == static_cast<size_t>(kMinCacheItems));
    const auto high = cacheLimitsFromConfig(
        Config::parse("[cache]\nmax_memory_mb = 999999\nmax_items = 999\n"));
    CHECK(high.maxBytes == static_cast<size_t>(kMaxCacheMemoryMB) * kMB);
    CHECK(high.maxItems == static_cast<size_t>(kMaxCacheItems));

    // 数として読めない値は既定のまま
    CHECK(cacheLimitsFromConfig(Config::parse("[cache]\nmax_memory_mb = abc\n")).maxBytes ==
          512 * kMB);

    // バイト上限に達したら、枚数上限に余裕があっても古い方から捨てる。
    // 1 枚 4MB / 上限 10MB なので 2 枚しか残らない
    BigDecoder decoder;
    ImageCache cache(decoder, ImageCacheLimits{.maxBytes = 10 * kMB, .maxItems = 8});
    std::mutex mutex;
    std::condition_variable cv;
    int decodedCount = 0;
    cache.setOnDecoded([&](const std::filesystem::path&) {
        std::lock_guard lock(mutex);
        ++decodedCount;
        cv.notify_all();
    });
    // 1 枚ずつ完了を待って積む(LRU の順序を確定させるため)
    const char* paths[] = {"a.png", "b.png", "c.png", "d.png"};
    for (int i = 0; i < 4; ++i) {
        cache.requestNow(paths[i]);
        std::unique_lock lock(mutex);
        CHECK(cv.wait_for(lock, std::chrono::seconds(5), [&] { return decodedCount >= i + 1; }));
    }
    CHECK(cache.tryGet("a.png") == nullptr);  // 古い 2 枚は捨てられている
    CHECK(cache.tryGet("b.png") == nullptr);
    CHECK(cache.tryGet("c.png") != nullptr);
    CHECK(cache.tryGet("d.png") != nullptr);
}

class FakeHost final : public IAppHost {
public:
    void requestRedraw() override {}
    void setTitle(const std::string& title) override { lastTitle = title; }
    void setFullscreen(bool enabled) override { fullscreen = enabled; }
    bool isFullscreen() const override { return fullscreen; }
    std::optional<std::filesystem::path> showOpenDialog() override { return std::nullopt; }
    std::optional<std::filesystem::path> showSaveDialog(
        const std::string& defaultFileName) override {
        ++saveDialogCount;
        lastDefaultName = defaultFileName;
        return savePath;
    }
    bool showConfirm(const std::string& message) override {
        ++confirmCount;
        lastConfirmMessage = message;
        return confirmAnswer;
    }
    std::optional<size_t> showContextMenu(const std::vector<MenuItem>& items, Point) override {
        ++menuCount;
        lastMenuItems = items;
        // キューがあれば先頭から順に応答する(設定→編集の連続選択のテスト用)
        if (!menuQueue.empty()) {
            const size_t choice = menuQueue.front();
            menuQueue.pop_front();
            return choice;
        }
        return menuChoice;
    }
    void setTextEditing(bool active, Point caretScreenPos, float caretHeightPx) override {
        textEditing = active;
        ++textEditingCalls;
        lastCaretPos = caretScreenPos;
        lastCaretHeight = caretHeightPx;
    }
    std::optional<uint32_t> showColorPicker(uint32_t initialRGB) override {
        ++colorPickerCount;
        lastColorPickerInitial = initialRGB;
        return colorChoice;
    }
    void startTimer(unsigned milliseconds) override { lastTimerMs = milliseconds; }
    void setFrameTimer(unsigned milliseconds) override {
        lastFrameTimerMs = milliseconds;
        ++frameTimerCalls;
    }
    void quit() override {}

    bool fullscreen = false;
    unsigned lastTimerMs = 0;
    unsigned lastFrameTimerMs = 0;  // アニメーション用タイマー(0 = 停止)
    int frameTimerCalls = 0;
    std::string lastTitle;
    std::optional<std::filesystem::path> savePath;  // 保存ダイアログの応答 (nullopt = キャンセル)
    int saveDialogCount = 0;
    std::string lastDefaultName;
    bool confirmAnswer = true;  // 確認ダイアログの応答 (false = 取りやめ)
    int confirmCount = 0;
    std::string lastConfirmMessage;
    std::optional<size_t> menuChoice;  // メニューの応答 (nullopt = キャンセル)
    std::deque<size_t> menuQueue;      // 空でなければ menuChoice より優先
    int menuCount = 0;
    std::vector<MenuItem> lastMenuItems;
    bool textEditing = false;    // setTextEditing が最後に通知した状態
    int textEditingCalls = 0;    // setTextEditing の呼び出し回数
    Point lastCaretPos;          // 最後に通知されたキャレット位置
    float lastCaretHeight = 0;   // 最後に通知されたキャレット高さ
    std::optional<uint32_t> colorChoice;    // 色ダイアログの応答 (nullopt = キャンセル)
    uint32_t lastColorPickerInitial = 0;
    int colorPickerCount = 0;
};

// 選択可能な末端項目(separator とサブメニュー親を除く)の数。index の対応確認用
size_t countMenuLeaves(const std::vector<MenuItem>& items) {
    size_t count = 0;
    for (const MenuItem& item : items) {
        if (item.separator) continue;
        if (!item.children.empty()) {
            count += countMenuLeaves(item.children);
        } else {
            ++count;
        }
    }
    return count;
}

// メニュー構造から見出しが prefix で始まる項目を探す(末端 index に依存せず中身を見る)
const MenuItem* findMenuItem(const std::vector<MenuItem>& items, std::string_view prefix) {
    for (const MenuItem& item : items) {
        if (item.text.starts_with(prefix)) return &item;
    }
    return nullptr;
}

// 注釈のない場所での右クリック(ドラッグなし)= ツール切り替えメニュー。
// leafIndex はメニューの末端項目: 0 トリミング, 1 文字認識, 2 矩形, 3 楕円, 4 矢印,
// 5 直線, 6 ペン, 7 マーカー, 8 連番マーカー, 9 テキスト,
// 10-16 太さ {1,2,3,5,8,12,20}, 17-23 文字サイズ {12,14,18,24,36,48,72},
// 24-32 フォント(候補9種。FakeAnnotationRasterizer は全て入っていることにする), 33 色,
// 34-38 塗りつぶし {0,64,128,191,255}, 39 塗りつぶしの色,
// 40-45 テキストの枠線 {0,1,2,3,5,8}, 46 枠線の色
constexpr Point kEmptySpot{600, 450};  // 画像・注釈の外(ツールメニューが開く位置)

// ツールメニューで末端項目を順に選ぶ。設定系(太さ・色など)を選ぶとメニューは
// 再表示されるので、最後にツールを選ぶか、選ばずに閉じる
void chooseInToolMenu(App& app, FakeHost& host, std::initializer_list<size_t> choices) {
    host.menuChoice = std::nullopt;  // menuQueue が尽きたらキャンセル扱いで閉じる
    host.menuQueue.assign(choices);
    app.onMouseDown(MouseButton::Right, kEmptySpot);
    app.onMouseUp(MouseButton::Right, kEmptySpot);  // ドラッグなし = メニュー
}

class FakeFileSystem final : public IFileSystem {
public:
    // 実装と同じ契約(名前昇順で返す)を守るため、files は昇順で入れること
    ListResult listImages(const std::filesystem::path& dir, const ListOptions& options) override {
        const std::vector<std::filesystem::path>& source =
            options.recursive && !recursiveFiles.empty() ? recursiveFiles : files;
        ListResult result;
        result.entries.reserve(source.size());
        for (size_t i = 0; i < source.size(); ++i) {
            FileEntry entry;
            entry.path = source[i];
            entry.relative = options.recursive ? source[i].lexically_relative(dir)
                                               : source[i].filename();
            if (entry.relative.empty()) entry.relative = source[i].filename();
            entry.lastWriteTick = i < ticks.size() ? ticks[i] : 0;
            entry.sizeBytes = i < sizes.size() ? sizes[i] : 0;
            result.entries.push_back(std::move(entry));
        }
        return result;
    }

    std::vector<std::filesystem::path> files;           ///< フォルダ直下の画像(名前昇順)
    std::vector<std::filesystem::path> recursiveFiles;  ///< 再帰時に返す一覧(空なら files)
    std::vector<int64_t> ticks;   ///< source と同じ添字の更新時刻(大小比較にだけ使う)
    std::vector<uint64_t> sizes;  ///< 同・ファイルサイズ
};

class FakeClipboard final : public IClipboard {
public:
    bool setImage(const DecodedImage& image) override {
        ++imageCount;
        lastWidth = image.width;
        lastHeight = image.height;
        lastPixels = image.pixels;
        return true;
    }
    bool setText(const std::string& text) override {
        lastText = text;
        return true;
    }
    bool setFiles(const std::vector<std::filesystem::path>& paths) override {
        if (paths.empty()) return false;
        ++filesCount;
        lastFiles = paths;
        return filesOk;
    }
    std::shared_ptr<DecodedImage> getImage() override { return pasteImage; }
    std::string getText() override { return pasteText; }

    int imageCount = 0;
    int filesCount = 0;
    bool filesOk = true;  // setFiles の戻り値(失敗経路の確認用)
    std::vector<std::filesystem::path> lastFiles;
    std::string pasteText;  // getText の応答
    uint32_t lastWidth = 0;
    uint32_t lastHeight = 0;
    std::vector<uint8_t> lastPixels;
    std::string lastText;
    std::shared_ptr<DecodedImage> pasteImage;  // getImage の応答 (nullptr = 画像なし)
};

class FakeEncoder final : public IImageEncoder {
public:
    bool supports(const std::filesystem::path& path) const override {
        const std::string ext = toLower(pathToUtf8(path.extension()));
        return std::find(supportedExtensions.begin(), supportedExtensions.end(), ext) !=
               supportedExtensions.end();
    }
    bool encode(const DecodedImage& image, const std::filesystem::path& path,
                const EncodeOptions& options) override {
        ++encodeCount;
        lastWidth = image.width;
        lastHeight = image.height;
        lastPath = path;
        lastPixels = image.pixels;
        lastJpegQuality = options.jpegQuality;
        return ok;
    }

    bool ok = true;
    int encodeCount = 0;
    uint32_t lastWidth = 0;
    uint32_t lastHeight = 0;
    std::vector<uint8_t> lastPixels;
    int lastJpegQuality = 0;
    std::filesystem::path lastPath;
    // EncoderWic / EncoderStb と同じ範囲(上書き保存の可否の判定に使われる)
    std::vector<std::string> supportedExtensions{".png", ".jpg", ".jpeg", ".bmp"};
};

class FakePrinter final : public IPrinter {
public:
    PrintStatus print(const DecodedImage& image, const std::string& jobName,
                      const PrintOptions& options) override {
        ++printCount;
        lastWidth = image.width;
        lastHeight = image.height;
        lastPixels = image.pixels;
        lastJobName = jobName;
        lastOptions = options;
        return status;
    }

    PrintStatus status = PrintStatus::Printed;  // 印刷の応答
    int printCount = 0;
    uint32_t lastWidth = 0;
    uint32_t lastHeight = 0;
    std::vector<uint8_t> lastPixels;
    std::string lastJobName;
    PrintOptions lastOptions;
};

// 指定サイズの不透明単色 overlay を返すテスト用ラスタライザ
// 決まった行を返す OCR エンジン。App から見た振る舞い(整形・クリップボード投入・
// 世代の食い違い)を検証するために使う
class FakeOcrEngine final : public IOcrEngine {
public:
    bool recognize(const DecodedImage& image, OcrResult* result, std::string* error) override {
        ++recognizeCount;
        lastWidth = image.width;
        lastHeight = image.height;
        lastFirstPixel = image.pixels.size() >= 4
                             ? std::array<uint8_t, 4>{image.pixels[0], image.pixels[1],
                                                      image.pixels[2], image.pixels[3]}
                             : std::array<uint8_t, 4>{};
        if (!ok) {
            if (error) *error = failureReason;
            return false;
        }
        result->lines = lines;
        result->language = language;
        return true;
    }

    bool ok = true;
    std::string failureReason = "テストの失敗理由";
    std::vector<OcrLine> lines;
    std::string language = "ja";
    int recognizeCount = 0;
    uint32_t lastWidth = 0;
    uint32_t lastHeight = 0;
    std::array<uint8_t, 4> lastFirstPixel{};
};

class FakeAnnotationRasterizer final : public IAnnotationRasterizer {
public:
    bool available() const override { return supported; }

    AnnotationOverlay rasterize(const AnnotationSpec& spec) override {
        ++rasterizeCount;
        lastSpec = spec;
        if (!ok) return {};
        auto image = std::make_shared<DecodedImage>();
        image->width = overlayWidth;
        image->height = overlayHeight;
        image->pixels.resize(static_cast<size_t>(overlayWidth) * overlayHeight * 4);
        for (size_t i = 0; i < image->pixels.size(); i += 4) {
            image->pixels[i] = 0;        // B
            image->pixels[i + 1] = 0;    // G
            image->pixels[i + 2] = 255;  // R
            image->pixels[i + 3] = 255;  // A
        }
        return {std::move(image), overlayX, overlayY};
    }

    // テキスト計測は「1 行・1 文字 kCharWidth px・行高 kLineHeight px」の単純な模型にする
    static constexpr float kCharWidth = 10.0f;
    static constexpr float kLineHeight = 20.0f;

    TextCaretMetrics caretMetrics(const AnnotationSpec& spec, size_t utf16Offset) override {
        const size_t length = utf8ToUtf16Offset(spec.text, spec.text.size());
        return {static_cast<float>(std::min(utf16Offset, length)) * kCharWidth, 0.0f,
                kLineHeight};
    }

    size_t hitTestTextOffset(const AnnotationSpec& spec, float localX, float) override {
        const size_t length = utf8ToUtf16Offset(spec.text, spec.text.size());
        if (localX <= 0) return 0;
        return std::min(static_cast<size_t>(localX / kCharWidth + 0.5f), length);
    }

    std::vector<TextRangeRect> selectionRects(const AnnotationSpec&, size_t utf16Begin,
                                              size_t utf16End) override {
        if (utf16End <= utf16Begin) return {};
        return {{static_cast<float>(utf16Begin) * kCharWidth, 0.0f,
                 static_cast<float>(utf16End) * kCharWidth, kLineHeight}};
    }

    // 既定では候補のフォントがすべて入っている環境として振る舞う
    // (メニューの末端 index を環境に依らず固定するため)
    bool hasFontFamily(const std::string& family) override {
        ++hasFontFamilyCount;
        return missingFonts.count(family) == 0;
    }

    bool ok = true;
    bool supported = true;  // false で SDL バックエンド相当(注釈を扱えない環境)にする
    int rasterizeCount = 0;
    uint32_t overlayWidth = 1;
    uint32_t overlayHeight = 1;
    int overlayX = 0;
    int overlayY = 0;
    AnnotationSpec lastSpec;
    std::set<std::string> missingFonts;  // 「入っていない」ことにするフォント
    int hasFontFamilyCount = 0;
};

void testAppClipboard() {
    FakeDecoder decoder;
    ImageCache cache(decoder);
    FakeHost host;
    FakeFileSystem fileSystem;
    FakeClipboard clipboard;
    FakeEncoder encoder;
    FakeAnnotationRasterizer rasterizer;
    FakeOcrEngine ocrEngine;
    OcrService ocrService(ocrEngine);
    ScanService scanService(fileSystem);
    FakePrinter printer;
    App app(host, fileSystem, cache, clipboard, encoder, rasterizer, ocrService, printer,
            scanService);

    // 画像を開いていない状態では何もコピーされない
    app.execute(Command::CopyImage);
    app.execute(Command::CopyPath);
    app.execute(Command::CopyFile);
    CHECK(clipboard.imageCount == 0);
    CHECK(clipboard.lastText.empty());
    CHECK(clipboard.filesCount == 0);

    // デコード完了を同期して待てるようにしてから画像を開く
    std::mutex mutex;
    std::condition_variable cv;
    bool decoded = false;
    cache.setOnDecoded([&](const std::filesystem::path&) {
        std::lock_guard lock(mutex);
        decoded = true;
        cv.notify_all();
    });
    const std::filesystem::path path = "C:/pics/a.png";
    fileSystem.files = {path};
    app.openPath(path);
    {
        std::unique_lock lock(mutex);
        CHECK(cv.wait_for(lock, std::chrono::seconds(5), [&] { return decoded; }));
    }
    app.onDecodeCompleted();  // 本来は UI スレッドへの PostMessage 経由
    CHECK(app.currentImage() != nullptr);

    app.execute(Command::CopyImage);
    CHECK(clipboard.imageCount == 1);
    CHECK(clipboard.lastWidth == 1);  // FakeDecoder は 1x1 を返す

    app.execute(Command::CopyPath);
    CHECK(clipboard.lastText == pathToUtf8(path));

    // ファイルのコピーは表示中の 1 件を渡す(実体は編集前のディスク上のファイル)
    app.execute(Command::CopyFile);
    CHECK(clipboard.filesCount == 1);
    CHECK(clipboard.lastFiles == std::vector<std::filesystem::path>{path});

    // 書き込みに失敗したらメッセージで知らせる
    clipboard.filesOk = false;
    app.execute(Command::CopyFile);
    CHECK(app.statusBar().leftText == "ファイルのコピーに失敗しました");
}

// 大きい画像(openPath 中にデコードが終わらない)でも「読み込み中」のまま止まらないこと。
// main_win.cpp と同じ順序 — setOnDecoded → openPath → 通知を受けて onDecodeCompleted
void testAppSlowDecode() {
    SlowDecoder decoder;
    ImageCache cache(decoder);
    FakeHost host;
    FakeFileSystem fileSystem;
    FakeClipboard clipboard;
    FakeEncoder encoder;
    FakeAnnotationRasterizer rasterizer;
    FakeOcrEngine ocrEngine;
    OcrService ocrService(ocrEngine);
    ScanService scanService(fileSystem);

    std::mutex mutex;
    std::condition_variable cv;
    bool notified = false;
    cache.setOnDecoded([&](const std::filesystem::path&) {
        std::lock_guard lock(mutex);
        notified = true;
        cv.notify_all();
    });

    FakePrinter printer;
    App app(host, fileSystem, cache, clipboard, encoder, rasterizer, ocrService, printer,
            scanService);
    app.onResize(800, 600);
    const std::filesystem::path path = "C:/pics/huge.jpeg";
    fileSystem.files = {path};
    app.openPath(path);

    // この時点ではまだデコード中(「読み込み中」表示)
    CHECK(app.currentImage() == nullptr);
    CHECK(host.lastTitle.find("(読み込み中)") != std::string::npos);

    {
        std::unique_lock lock(mutex);
        CHECK(cv.wait_for(lock, std::chrono::seconds(5), [&] { return notified; }));
    }
    app.onDecodeCompleted();
    // 通知後は必ず画像が出ていること(ここで止まると「読み込み中」のまま固まる)
    CHECK(app.currentImage() != nullptr);
    CHECK(host.lastTitle.find("(読み込み中)") == std::string::npos);
}

void testAppStatusBar() {
    FakeDecoder decoder;
    ImageCache cache(decoder);
    FakeHost host;
    FakeFileSystem fileSystem;
    FakeClipboard clipboard;
    FakeEncoder encoder;
    FakeAnnotationRasterizer rasterizer;
    FakeOcrEngine ocrEngine;
    OcrService ocrService(ocrEngine);
    ScanService scanService(fileSystem);
    FakePrinter printer;
    App app(host, fileSystem, cache, clipboard, encoder, rasterizer, ocrService, printer,
            scanService);
    app.onResize(800, 600);

    // 画像なし: バーは表示されるが左右とも空
    StatusBarView bar = app.statusBar();
    CHECK(bar.visible);
    CHECK(bar.height > 0);
    CHECK(bar.leftText.empty());
    CHECK(bar.rightText.empty());

    // 画像 (FakeDecoder の 1x1 黒) を開く
    std::mutex mutex;
    std::condition_variable cv;
    bool decoded = false;
    cache.setOnDecoded([&](const std::filesystem::path&) {
        std::lock_guard lock(mutex);
        decoded = true;
        cv.notify_all();
    });
    const std::filesystem::path path = "C:/pics/black.png";
    fileSystem.files = {path};
    app.openPath(path);
    {
        std::unique_lock lock(mutex);
        CHECK(cv.wait_for(lock, std::chrono::seconds(5), [&] { return decoded; }));
    }
    app.onDecodeCompleted();
    CHECK(app.currentImage() != nullptr);
    CHECK(app.statusBar().leftText == "1 x 1 px  |  ツール: 矩形");

    // 1x1 画像はビューポート 800x(600-26) の中央 (400, 287) に等倍表示される。
    // その位置にカーソルを置くとピクセル (0,0) の座標と色が出る
    app.onMouseMove({400, 287});
    CHECK(app.statusBar().rightText == "(0, 0)  #000000  RGB(0, 0, 0)");

    // ステータスバー上・画像外では表示しない
    app.onMouseMove({400, 590});
    CHECK(app.statusBar().rightText.empty());
    app.onMouseMove({400, 287});
    CHECK(!app.statusBar().rightText.empty());
    app.onMouseLeave();
    CHECK(app.statusBar().rightText.empty());

    // コピーで通知が出て、タイマー満了で画像情報表示に戻る
    app.execute(Command::CopyImage);
    CHECK(app.statusBar().leftText == "画像をクリップボードにコピーしました");
    CHECK(host.lastTimerMs == 3000);
    app.execute(Command::CopyPath);
    CHECK(app.statusBar().leftText == "パスをコピーしました: " + pathToUtf8(path));
    app.onTimer();
    CHECK(app.statusBar().leftText == "1 x 1 px  |  ツール: 矩形");

    // トグルとフルスクリーンで非表示になる
    app.execute(Command::ToggleStatusBar);
    CHECK(!app.statusBar().visible);
    app.execute(Command::ToggleStatusBar);
    CHECK(app.statusBar().visible);
    host.fullscreen = true;
    CHECK(!app.statusBar().visible);
    host.fullscreen = false;

    // タイトルバーには常にバージョンと git SHA-1 が付く
    const std::string appName = std::format("Blinker v{} ({})", kAppVersion, kAppGitSha);
    CHECK(host.lastTitle.ends_with(" - " + appName));

    // デコード失敗時は、失敗した段階と HRESULT までステータスバーに出す
    decoded = false;
    const std::filesystem::path bad = "C:/pics/fail.png";
    fileSystem.files = {bad};
    app.openPath(bad);
    {
        std::unique_lock lock(mutex);
        CHECK(cv.wait_for(lock, std::chrono::seconds(5), [&] { return decoded; }));
    }
    app.onDecodeCompleted();
    CHECK(app.currentImage() == nullptr);
    CHECK(app.statusBar().leftText == "読み込み失敗: ピクセル取得 (0x88982F50)");
    CHECK(host.lastTitle.find("(読み込み失敗)") != std::string::npos);
}

// 画像を読まずに起動 → 貼り付け、の状態で移動系を叩いても貼り付け画像を失わないこと。
// 一覧が空だと戻る先が無く、捨ててしまうと二度と表示に戻せない
void testAppPasteWithoutFolder() {
    FakeDecoder decoder;
    ImageCache cache(decoder);
    FakeHost host;
    FakeFileSystem fileSystem;
    FakeClipboard clipboard;
    FakeEncoder encoder;
    FakeAnnotationRasterizer rasterizer;
    FakeOcrEngine ocrEngine;
    OcrService ocrService(ocrEngine);
    ScanService scanService(fileSystem);
    FakePrinter printer;
    App app(host, fileSystem, cache, clipboard, encoder, rasterizer, ocrService, printer,
            scanService);
    app.onResize(800, 600);

    auto pasted = std::make_shared<DecodedImage>();
    pasted->width = 2;
    pasted->height = 1;
    pasted->pixels = {0, 0, 255, 255, 0, 255, 0, 255};
    clipboard.pasteImage = pasted;
    app.execute(Command::PasteImage);
    CHECK(app.currentImage() && app.currentImage()->width == 2);
    CHECK(host.lastTitle.find("(クリップボード)") == 0);

    for (const Command command : {Command::NextImage, Command::PrevImage, Command::FirstImage,
                                  Command::LastImage}) {
        app.execute(command);
        CHECK(app.currentImage() && app.currentImage()->width == 2);
        CHECK(host.lastTitle.find("(クリップボード)") == 0);
    }
    // オーバーレイ矢印も出ない(押せてしまうと同じことが起きる)
    app.onMouseMove({790, 300});
    CHECK(!app.navArrows().arrows.next.visible);
    CHECK(!app.navArrows().arrows.prev.visible);
}

// 水平ホイール(チルト)の誤爆対策。トラックボールでは縦スクロール中に微小な横成分が
// 混ざり続けるため、しきい値・軸ロック・サイドバーの 3 つで弾く
void testAppWheelHorizontal() {
    FakeDecoder decoder;
    ImageCache cache(decoder);
    FakeHost host;
    FakeFileSystem fileSystem;
    FakeClipboard clipboard;
    FakeEncoder encoder;
    FakeAnnotationRasterizer rasterizer;
    FakeOcrEngine ocrEngine;
    OcrService ocrService(ocrEngine);
    ScanService scanService(fileSystem);
    FakePrinter printer;
    App app(host, fileSystem, cache, clipboard, encoder, rasterizer, ocrService, printer,
            scanService);
    app.onResize(800, 600);
    for (int i = 1; i <= 6; ++i) {
        fileSystem.files.push_back(std::format("C:/pics/f{:02}.png", i));
    }
    std::mutex mutex;
    std::condition_variable cv;
    bool decoded = false;
    cache.setOnDecoded([&](const std::filesystem::path&) {
        std::lock_guard lock(mutex);
        decoded = true;
        cv.notify_all();
    });
    app.openPath(fileSystem.files[0]);
    {
        std::unique_lock lock(mutex);
        CHECK(cv.wait_for(lock, std::chrono::seconds(5), [&] { return decoded; }));
    }
    app.onDecodeCompleted();
    const auto showing = [&host](std::string_view name) {
        return host.lastTitle.find(name) != std::string::npos;
    };
    CHECK(showing("f01.png"));

    // 既定は垂直と同じ 1 ノッチ 1 枚(普通のチルトホイールは倒すたびに切り替わる)
    app.onWheelHorizontal(1.0f, {500, 300});
    CHECK(showing("f02.png"));

    // 垂直ホイールが来たら横の貯金は捨てる(縦スクロール中の横成分を積ませない)
    app.onWheelHorizontal(0.5f, {500, 300});
    app.onWheel(-1.0f, {500, 300});  // 画像の上なのでズーム(遷移はしない)
    CHECK(showing("f02.png"));
    app.onWheelHorizontal(0.5f, {500, 300});
    CHECK(showing("f02.png"));  // 捨てられているので合計 1 ノッチでも足りない
    app.onWheelHorizontal(0.5f, {500, 300});
    CHECK(showing("f03.png"));  // 貯金そのものは効いている

    // サイドバー(ここでは操作一覧)の上では効かず、貯金も残さない
    app.execute(Command::ToggleHelp);
    CHECK(nearly(app.sidebar().width, 300));
    app.onWheelHorizontal(5.0f, {100, 300});
    CHECK(showing("f03.png"));
    app.onWheelHorizontal(0.5f, {500, 300});
    CHECK(showing("f03.png"));
    app.execute(Command::ToggleHelp);
    app.onWheel(-1.0f, {500, 300});  // 貯金を捨てて次の確認へ
    app.onWheelHorizontal(1.0f, {500, 300});
    CHECK(showing("f04.png"));

    // しきい値は設定で上げられる(トラックボールでの誤爆対策)
    app.applyConfig(Config::parse("[mouse]\nwheel_horizontal_threshold = 2\n"));
    app.onWheelHorizontal(1.0f, {500, 300});
    CHECK(showing("f04.png"));
    app.onWheelHorizontal(1.0f, {500, 300});
    CHECK(showing("f05.png"));
    // 範囲外はクランプされる(0 は 1 として扱う)
    app.applyConfig(Config::parse("[mouse]\nwheel_horizontal_threshold = 0\n"));
    app.onWheelHorizontal(-1.0f, {500, 300});
    CHECK(showing("f04.png"));
}

void testAppPasteSave() {
    FakeDecoder decoder;
    ImageCache cache(decoder);
    FakeHost host;
    FakeFileSystem fileSystem;
    FakeClipboard clipboard;
    FakeEncoder encoder;
    FakeAnnotationRasterizer rasterizer;
    FakeOcrEngine ocrEngine;
    OcrService ocrService(ocrEngine);
    ScanService scanService(fileSystem);
    FakePrinter printer;
    App app(host, fileSystem, cache, clipboard, encoder, rasterizer, ocrService, printer,
            scanService);
    app.onResize(800, 600);

    // 画像なしでの保存: ダイアログは開かずメッセージ
    app.execute(Command::SaveImageAs);
    CHECK(host.saveDialogCount == 0);
    CHECK(app.statusBar().leftText == "保存する画像がありません");

    // クリップボードが空のときの貼り付け
    app.execute(Command::PasteImage);
    CHECK(app.currentImage() == nullptr);
    CHECK(app.statusBar().leftText == "クリップボードに画像がありません");

    // フォルダの画像 (FakeDecoder の 1x1) を開く
    std::mutex mutex;
    std::condition_variable cv;
    bool decoded = false;
    cache.setOnDecoded([&](const std::filesystem::path&) {
        std::lock_guard lock(mutex);
        decoded = true;
        cv.notify_all();
    });
    const std::filesystem::path path = "C:/pics/a.png";
    fileSystem.files = {path};
    app.openPath(path);
    {
        std::unique_lock lock(mutex);
        CHECK(cv.wait_for(lock, std::chrono::seconds(5), [&] { return decoded; }));
    }
    app.onDecodeCompleted();
    CHECK(app.currentImage() && app.currentImage()->width == 1);

    // 2x1 のクリップボード画像を貼り付け
    auto pasted = std::make_shared<DecodedImage>();
    pasted->width = 2;
    pasted->height = 1;
    pasted->pixels = {0, 0, 255, 255, 0, 255, 0, 255};
    clipboard.pasteImage = pasted;
    app.onTimer();  // 直前の通知メッセージを消しておく
    app.execute(Command::PasteImage);
    CHECK(app.currentImage() && app.currentImage()->width == 2);
    CHECK(host.lastTitle.find("(クリップボード)") == 0);
    CHECK(app.statusBar().leftText == "2 x 1 px  |  ツール: 矩形");

    // デコード完了通知が来ても貼り付け画像は上書きされない
    app.onDecodeCompleted();
    CHECK(app.currentImage()->width == 2);

    // 貼り付け表示中はパスのコピーを拒否(一覧のパスとは無関係のため)
    app.execute(Command::CopyPath);
    CHECK(app.statusBar().leftText == "コピーするパスがありません");

    // ファイルのコピーも同様(貼り付け画像に対応するファイルは存在しない)
    app.execute(Command::CopyFile);
    CHECK(app.statusBar().leftText == "コピーするファイルがありません");
    CHECK(clipboard.filesCount == 0);

    // 貼り付け画像の保存: 既定名は「クリップボード.png」
    host.savePath = std::filesystem::path("C:/out/pasted.png");
    app.execute(Command::SaveImageAs);
    CHECK(host.lastDefaultName == "クリップボード.png");
    CHECK(encoder.lastPath == std::filesystem::path("C:/out/pasted.png"));
    CHECK(encoder.lastWidth == 2);
    CHECK(app.statusBar().leftText == "保存しました: C:/out/pasted.png");

    // 次へ移動でフォルダ一覧の表示に戻る(1枚しかなくても)
    app.execute(Command::NextImage);
    CHECK(app.currentImage() && app.currentImage()->width == 1);
    CHECK(host.lastTitle.find("a.png") != std::string::npos);

    // 通常画像の保存: 既定名は元ファイル名の .png 置き換え
    host.savePath = std::filesystem::path("C:/out/copy.jpg");
    app.execute(Command::SaveImageAs);
    CHECK(host.lastDefaultName == "a.png");
    CHECK(encoder.lastWidth == 1);
    CHECK(app.statusBar().leftText == "保存しました: C:/out/copy.jpg");

    // ダイアログのキャンセル: エンコードもメッセージも発生しない
    app.onTimer();  // 前のメッセージを消す
    host.savePath.reset();
    const int encodeCountBefore = encoder.encodeCount;
    app.execute(Command::SaveImageAs);
    CHECK(encoder.encodeCount == encodeCountBefore);
    CHECK(app.statusBar().leftText == "1 x 1 px  |  ツール: 矩形");

    // 保存失敗
    encoder.ok = false;
    host.savePath = std::filesystem::path("C:/out/x.png");
    app.execute(Command::SaveImageAs);
    CHECK(app.statusBar().leftText == "保存に失敗しました: C:/out/x.png");
}

void testAppSaveOverwrite() {
    FakeDecoder decoder;
    ImageCache cache(decoder);
    FakeHost host;
    FakeFileSystem fileSystem;
    FakeClipboard clipboard;
    FakeEncoder encoder;
    FakeAnnotationRasterizer rasterizer;
    FakeOcrEngine ocrEngine;
    OcrService ocrService(ocrEngine);
    ScanService scanService(fileSystem);
    FakePrinter printer;
    App app(host, fileSystem, cache, clipboard, encoder, rasterizer, ocrService, printer,
            scanService);
    app.onResize(800, 600);
    app.applyConfig(Config::parse("[save]\njpeg_quality = 55\n"));

    std::mutex mutex;
    std::condition_variable cv;
    bool decoded = false;
    cache.setOnDecoded([&](const std::filesystem::path&) {
        std::lock_guard lock(mutex);
        decoded = true;
        cv.notify_all();
    });
    const std::filesystem::path path = "C:/pics/a.png";
    fileSystem.files = {path};
    app.openPath(path);
    {
        std::unique_lock lock(mutex);
        CHECK(cv.wait_for(lock, std::chrono::seconds(5), [&] { return decoded; }));
    }
    app.onDecodeCompleted();
    CHECK(app.currentImage() != nullptr);

    // 上書き保存: 保存先は尋ねず、確認だけ取って元のファイルへ書く
    app.execute(Command::SaveImage);
    CHECK(host.confirmCount == 1);
    CHECK(host.lastConfirmMessage.find("a.png") != std::string::npos);
    CHECK(host.saveDialogCount == 0);
    CHECK(encoder.lastPath == path);
    CHECK(encoder.lastJpegQuality == 55);  // [save] jpeg_quality が届いている
    CHECK(app.statusBar().leftText == "上書き保存しました: C:/pics/a.png");
    // 書き換えたファイルのキャッシュは捨てる(戻ってきたときに読み直させる)
    CHECK(cache.tryGet(path) == nullptr);

    // 確認で取りやめたら書かない
    host.confirmAnswer = false;
    const int encodeCountBefore = encoder.encodeCount;
    app.execute(Command::SaveImage);
    CHECK(host.confirmCount == 2);
    CHECK(encoder.encodeCount == encodeCountBefore);
    host.confirmAnswer = true;

    // confirm_overwrite = false なら聞かずに書く
    app.applyConfig(Config::parse("[save]\nconfirm_overwrite = false\n"));
    app.execute(Command::SaveImage);
    CHECK(host.confirmCount == 2);
    CHECK(encoder.encodeCount == encodeCountBefore + 1);

    // 保存できない形式は、ファイルへ手を付ける前に断る
    encoder.supportedExtensions.clear();
    app.execute(Command::SaveImage);
    CHECK(encoder.encodeCount == encodeCountBefore + 1);
    CHECK(app.statusBar().leftText.find("上書き保存に対応していない形式") != std::string::npos);
    CHECK(app.statusBar().leftText.find(".png") != std::string::npos);
    encoder.supportedExtensions = {".png"};

    // 貼り付け画像には上書き先が無いので、名前を付けて保存へ回る
    auto pasted = std::make_shared<DecodedImage>();
    pasted->width = 2;
    pasted->height = 1;
    pasted->pixels = {0, 0, 255, 255, 0, 255, 0, 255};  // 赤・緑 (PBGRA)
    clipboard.pasteImage = pasted;
    app.execute(Command::PasteImage);
    host.savePath = std::filesystem::path("C:/out/p.png");
    app.execute(Command::SaveImage);
    CHECK(host.saveDialogCount == 1);
    CHECK(host.confirmCount == 2);  // 上書きではないので確認しない
    CHECK(encoder.lastPath == std::filesystem::path("C:/out/p.png"));
    CHECK(encoder.lastWidth == 2 && encoder.lastHeight == 1);
    CHECK(app.statusBar().leftText == "保存しました: C:/out/p.png");

    // 表示回転は保存・コピーにも効く(2x1 を右 90 度に回すと 1x2 になる)
    app.execute(Command::RotateCW);  // 0 → 90
    app.execute(Command::SaveImage);
    CHECK(encoder.lastWidth == 1 && encoder.lastHeight == 2);
    app.execute(Command::CopyImage);
    CHECK(clipboard.lastWidth == 1 && clipboard.lastHeight == 2);
    CHECK(clipboard.lastPixels == std::vector<uint8_t>({0, 0, 255, 255, 0, 255, 0, 255}));

    // 左 90 度は右 90 度と並びが逆になる
    app.execute(Command::RotateCCW);  // 90 → 0
    app.execute(Command::RotateCCW);  // 0 → 270 (= 左 90 度)
    app.execute(Command::CopyImage);
    CHECK(clipboard.lastWidth == 1 && clipboard.lastHeight == 2);
    CHECK(clipboard.lastPixels == std::vector<uint8_t>({0, 255, 0, 255, 0, 0, 255, 255}));

    // 180 度は縦横が変わらないので並びで見る
    app.execute(Command::RotateCW);  // 270 → 0
    app.execute(Command::RotateCW);
    app.execute(Command::RotateCW);  // 0 → 180
    app.execute(Command::CopyImage);
    CHECK(clipboard.lastWidth == 2 && clipboard.lastHeight == 1);
    CHECK(clipboard.lastPixels == std::vector<uint8_t>({0, 255, 0, 255, 0, 0, 255, 255}));

    // 回転を戻せば元のままコピーされる
    app.execute(Command::RotateCW);
    app.execute(Command::RotateCW);  // 180 → 0
    app.execute(Command::CopyImage);
    CHECK(clipboard.lastWidth == 2 && clipboard.lastHeight == 1);
    CHECK(clipboard.lastPixels == pasted->pixels);
}

// 元が大きすぎて縮小して取り込まれた画像を返すデコーダ(巨大画像の扱いを試す)
class DownscaledDecoder final : public IImageDecoder {
public:
    std::shared_ptr<DecodedImage> decode(const std::filesystem::path&,
                                         std::string* = nullptr) override {
        auto image = std::make_shared<DecodedImage>();
        image->width = 2;
        image->height = 1;
        image->pixels = {0, 0, 0, 255, 255, 255, 255, 255};
        image->sourceWidth = 40000;
        image->sourceHeight = 20000;
        return image;
    }
};

void testAppSaveDownscaled() {
    DownscaledDecoder decoder;
    ImageCache cache(decoder);
    FakeHost host;
    FakeFileSystem fileSystem;
    FakeClipboard clipboard;
    FakeEncoder encoder;
    FakeAnnotationRasterizer rasterizer;
    FakeOcrEngine ocrEngine;
    OcrService ocrService(ocrEngine);
    ScanService scanService(fileSystem);
    FakePrinter printer;
    App app(host, fileSystem, cache, clipboard, encoder, rasterizer, ocrService, printer,
            scanService);
    app.onResize(800, 600);

    std::mutex mutex;
    std::condition_variable cv;
    bool decoded = false;
    cache.setOnDecoded([&](const std::filesystem::path&) {
        std::lock_guard lock(mutex);
        decoded = true;
        cv.notify_all();
    });
    const std::filesystem::path path = "C:/pics/huge.png";
    fileSystem.files = {path};
    app.openPath(path);
    {
        std::unique_lock lock(mutex);
        CHECK(cv.wait_for(lock, std::chrono::seconds(5), [&] { return decoded; }));
    }
    app.onDecodeCompleted();
    CHECK(app.currentImage() != nullptr);
    CHECK(app.currentImage()->downscaled());

    // 縮小表示中であることが分かる(ズーム率が元の大きさに対する比ではなくなるため)
    CHECK(app.statusBar().leftText.find("元 40000 x 20000 を縮小表示") != std::string::npos);

    // 上書き保存は断る。確認ダイアログを出す前に断ること(聞いてから断るのは不親切)
    app.execute(Command::SaveImage);
    CHECK(host.confirmCount == 0);
    CHECK(encoder.encodeCount == 0);
    CHECK(app.statusBar().leftText.find("上書きすると画素が失われる") != std::string::npos);
    CHECK(app.statusBar().leftText.find("40000 x 20000") != std::string::npos);

    // 名前を付けて保存は許すが、小さくなることを伝える
    host.savePath = std::filesystem::path("C:/out/huge.png");
    app.execute(Command::SaveImageAs);
    CHECK(encoder.encodeCount == 1);
    CHECK(encoder.lastPath == std::filesystem::path("C:/out/huge.png"));
    CHECK(app.statusBar().leftText == "保存しました(表示用に縮小した 2 x 1 で): C:/out/huge.png");

    // 名前を付けて保存で元のファイルを選び直した場合も、結果は上書きなので断る
    host.savePath = path;
    app.execute(Command::SaveImageAs);
    CHECK(encoder.encodeCount == 1);
    CHECK(app.statusBar().leftText.find("上書きすると画素が失われる") != std::string::npos);
}

// 遅延カラーマネジメントを模したデコーダ。1 回目は未変換 (colorPending)、
// decodeColorManaged で変換済みの画素を返す
class LazyColorDecoder final : public IImageDecoder {
public:
    std::shared_ptr<DecodedImage> decode(const std::filesystem::path& path,
                                         std::string* error = nullptr) override {
        ++decodeCount;
        if (pathToUtf8(path).find("plain") != std::string::npos) {
            return makeImage(0, false);  // プロファイルなし: 読み直しは起きない
        }
        if (pathToUtf8(path).find("fail") != std::string::npos) {
            if (error) *error = "デコード失敗";
            return nullptr;
        }
        return makeImage(10, true);
    }

    std::shared_ptr<DecodedImage> decodeColorManaged(const std::filesystem::path& path,
                                                     std::string* = nullptr) override {
        ++managedCount;
        // 差し替え前の状態を確かめられるよう、テストが許すまで待つ
        {
            std::unique_lock lock(gateMutex_);
            gate_.wait_for(lock, std::chrono::seconds(5), [&] { return allowed_; });
        }
        if (pathToUtf8(path).find("noprofile_after") != std::string::npos) {
            return nullptr;  // 変換できなかった場合(最初の結果を使い続ける)
        }
        auto image = makeImage(200, false);
        image->colorConverted = true;
        return image;
    }

    /// @brief 待たせてある読み直しを進ませる。
    void allowManaged() {
        {
            std::lock_guard lock(gateMutex_);
            allowed_ = true;
        }
        gate_.notify_all();
    }

    std::atomic<int> decodeCount{0};
    std::atomic<int> managedCount{0};

private:
    static std::shared_ptr<DecodedImage> makeImage(uint8_t blue, bool pending) {
        auto image = std::make_shared<DecodedImage>();
        image->width = 1;
        image->height = 1;
        image->pixels = {blue, 0, 0, 255};
        image->colorPending = pending;
        return image;
    }

    std::mutex gateMutex_;
    std::condition_variable gate_;
    bool allowed_ = false;
};

void testImageCacheColorRefine() {
    LazyColorDecoder decoder;
    ImageCache cache(decoder);
    std::mutex mutex;
    std::condition_variable cv;
    int notifications = 0;
    cache.setOnDecoded([&](const std::filesystem::path&) {
        std::lock_guard lock(mutex);
        ++notifications;
        cv.notify_all();
    });
    const auto waitFor = [&](int count) {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, std::chrono::seconds(5), [&] { return notifications >= count; });
    };

    // プロファイル付き: 1 回目は未変換で届き、読み直しの完了で差し替わる
    cache.requestNow("p3.jpg");
    CHECK(waitFor(1));
    const auto first = cache.tryGet("p3.jpg");
    CHECK(first != nullptr);
    CHECK(first->pixels[0] == 10);
    CHECK(first->colorPending);
    decoder.allowManaged();
    CHECK(waitFor(2));  // 読み直しの通知も来る
    const auto refined = cache.tryGet("p3.jpg");
    CHECK(refined != nullptr);
    CHECK(refined != first);  // 別の画像に差し替わっている
    CHECK(refined->pixels[0] == 200);
    CHECK(refined->colorConverted);
    CHECK(!refined->colorPending);
    CHECK(decoder.managedCount == 1);

    // 差し替えは一度だけ(何度も読み直さない)
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(decoder.managedCount == 1);
    CHECK(cache.tryGet("p3.jpg") == refined);

    // プロファイルなし: 読み直しは起きない(通知も 1 回だけ)
    cache.requestNow("plain.jpg");
    CHECK(waitFor(3));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(decoder.managedCount == 1);
    CHECK(notifications == 3);

    // 変換できなかった場合は最初の結果を使い続け、二度は試さない
    cache.requestNow("noprofile_after.jpg");
    CHECK(waitFor(4));
    const auto kept = cache.tryGet("noprofile_after.jpg");
    CHECK(kept != nullptr && kept->pixels[0] == 10);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(decoder.managedCount == 2);   // 一度は試した
    CHECK(notifications == 4);          // 差し替えていないので通知は増えない
    CHECK(cache.tryGet("noprofile_after.jpg") == kept);

    // デコード失敗したパスは読み直しの対象にならない
    cache.requestNow("fail.jpg");
    CHECK(waitFor(5));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(decoder.managedCount == 2);
}

void testAppAdoptsRefinedImage() {
    LazyColorDecoder decoder;
    ImageCache cache(decoder);
    FakeHost host;
    FakeFileSystem fileSystem;
    FakeClipboard clipboard;
    FakeEncoder encoder;
    FakeAnnotationRasterizer rasterizer;
    FakeOcrEngine ocrEngine;
    OcrService ocrService(ocrEngine);
    ScanService scanService(fileSystem);
    FakePrinter printer;
    App app(host, fileSystem, cache, clipboard, encoder, rasterizer, ocrService, printer,
            scanService);
    app.onResize(800, 600);

    std::mutex mutex;
    std::condition_variable cv;
    int notifications = 0;
    cache.setOnDecoded([&](const std::filesystem::path&) {
        std::lock_guard lock(mutex);
        ++notifications;
        cv.notify_all();
    });
    const std::filesystem::path path = "C:/pics/p3.jpg";
    fileSystem.files = {path};
    app.openPath(path);
    {
        std::unique_lock lock(mutex);
        CHECK(cv.wait_for(lock, std::chrono::seconds(5), [&] { return notifications >= 1; }));
    }
    app.onDecodeCompleted();
    CHECK(app.currentImage() != nullptr);
    CHECK(app.currentImage()->pixels[0] == 10);  // まず未変換のまま表示する

    // 読み直しが済んだら、ズーム状態を保ったまま画素だけ差し替わる
    app.execute(Command::ZoomIn);
    const float zoom = app.zoom();
    decoder.allowManaged();
    {
        std::unique_lock lock(mutex);
        CHECK(cv.wait_for(lock, std::chrono::seconds(5), [&] { return notifications >= 2; }));
    }
    app.onDecodeCompleted();
    CHECK(app.currentImage()->pixels[0] == 200);
    CHECK(app.currentImage()->colorConverted);
    CHECK(app.zoom() == zoom);

    // 上書き保存の確認には、色空間の情報が失われることが出る
    encoder.supportedExtensions = {".jpg"};
    app.execute(Command::SaveImage);
    CHECK(host.lastConfirmMessage.find("元の色空間の情報は失われます") != std::string::npos);
}

// 埋め込みプロファイルから sRGB へ変換して取り込まれた画像を返すデコーダ
class ColorConvertedDecoder final : public IImageDecoder {
public:
    std::shared_ptr<DecodedImage> decode(const std::filesystem::path&,
                                         std::string* = nullptr) override {
        auto image = std::make_shared<DecodedImage>();
        image->width = 1;
        image->height = 1;
        image->pixels = {10, 20, 30, 255};
        image->colorConverted = true;
        return image;
    }
};

void testAppSaveColorConverted() {
    ColorConvertedDecoder decoder;
    ImageCache cache(decoder);
    FakeHost host;
    FakeFileSystem fileSystem;
    FakeClipboard clipboard;
    FakeEncoder encoder;
    FakeAnnotationRasterizer rasterizer;
    FakeOcrEngine ocrEngine;
    OcrService ocrService(ocrEngine);
    ScanService scanService(fileSystem);
    FakePrinter printer;
    App app(host, fileSystem, cache, clipboard, encoder, rasterizer, ocrService, printer,
            scanService);
    app.onResize(800, 600);

    std::mutex mutex;
    std::condition_variable cv;
    bool decoded = false;
    cache.setOnDecoded([&](const std::filesystem::path&) {
        std::lock_guard lock(mutex);
        decoded = true;
        cv.notify_all();
    });
    const std::filesystem::path path = "C:/pics/p3.jpg";
    fileSystem.files = {path};
    app.openPath(path);
    {
        std::unique_lock lock(mutex);
        CHECK(cv.wait_for(lock, std::chrono::seconds(5), [&] { return decoded; }));
    }
    app.onDecodeCompleted();
    CHECK(app.currentImage() != nullptr);

    // 上書き自体は許すが、色空間の情報が失われることを確認ダイアログで伝える
    encoder.supportedExtensions = {".jpg"};
    app.execute(Command::SaveImage);
    CHECK(host.confirmCount == 1);
    CHECK(host.lastConfirmMessage.find("元の色空間の情報は失われます") != std::string::npos);
    CHECK(encoder.encodeCount == 1);
    // 確認を出さない設定でも、保存後のメッセージには残る
    CHECK(app.statusBar().leftText == "上書き保存しました(sRGB へ変換した色で): C:/pics/p3.jpg");
}

void testPrintLayout() {
    // 横長の画像を横長の領域へ: 幅いっぱいに広げ、上下は中央
    PrintPlacement placement = layoutPrintImage(200, 100, 1000, 800, true);
    CHECK(!placement.rotated);
    CHECK(placement.width == 1000);
    CHECK(placement.height == 500);
    CHECK(placement.x == 0);
    CHECK(placement.y == 150);

    // 画像が小さくても用紙いっぱいに拡大する(原寸で刷ると 600dpi では切手大になる)
    placement = layoutPrintImage(10, 10, 1000, 800, false);
    CHECK(placement.width == 800);
    CHECK(placement.height == 800);
    CHECK(placement.x == 100);
    CHECK(placement.y == 0);

    // 横長の画像 × 縦長の用紙: 回した方が大きく刷れるので回す
    placement = layoutPrintImage(200, 100, 800, 1000, true);
    CHECK(placement.rotated);
    CHECK(placement.width == 500);
    CHECK(placement.height == 1000);
    CHECK(placement.x == 150);
    CHECK(placement.y == 0);

    // auto_rotate = false なら向きはそのまま(用紙は余る)
    placement = layoutPrintImage(200, 100, 800, 1000, false);
    CHECK(!placement.rotated);
    CHECK(placement.width == 800);
    CHECK(placement.height == 400);

    // 画像と用紙の向きが同じなら回さない
    placement = layoutPrintImage(100, 200, 800, 1000, true);
    CHECK(!placement.rotated);
    CHECK(placement.width == 500);
    CHECK(placement.height == 1000);

    // 正方形は回しても大きくならないので回さない
    CHECK(!layoutPrintImage(100, 100, 800, 1000, true).rotated);

    // 不正な入力では配置を返さない(呼び出し側は印刷を諦める)
    CHECK(layoutPrintImage(0, 100, 800, 1000, true).width == 0);
    CHECK(layoutPrintImage(100, 0, 800, 1000, true).width == 0);
    CHECK(layoutPrintImage(100, 100, 0, 1000, true).width == 0);
    CHECK(layoutPrintImage(100, 100, 800, 0, true).width == 0);
}

void testAppPrint() {
    FakeDecoder decoder;
    ImageCache cache(decoder);
    FakeHost host;
    FakeFileSystem fileSystem;
    FakeClipboard clipboard;
    FakeEncoder encoder;
    FakeAnnotationRasterizer rasterizer;
    FakeOcrEngine ocrEngine;
    OcrService ocrService(ocrEngine);
    ScanService scanService(fileSystem);
    FakePrinter printer;
    App app(host, fileSystem, cache, clipboard, encoder, rasterizer, ocrService, printer,
            scanService);
    app.onResize(800, 600);

    // 画像なし: プリンタには渡さずメッセージだけ
    app.execute(Command::PrintImage);
    CHECK(printer.printCount == 0);
    CHECK(app.statusBar().leftText == "印刷する画像がありません");

    // フォルダの画像 (FakeDecoder の 1x1) を開く
    std::mutex mutex;
    std::condition_variable cv;
    bool decoded = false;
    cache.setOnDecoded([&](const std::filesystem::path&) {
        std::lock_guard lock(mutex);
        decoded = true;
        cv.notify_all();
    });
    const std::filesystem::path path = "C:/pics/a.png";
    fileSystem.files = {path};
    app.openPath(path);
    {
        std::unique_lock lock(mutex);
        CHECK(cv.wait_for(lock, std::chrono::seconds(5), [&] { return decoded; }));
    }
    app.onDecodeCompleted();

    // ジョブ名はファイル名。設定を書いていなければ既定の余白・自動回転で渡る
    app.execute(Command::PrintImage);
    CHECK(printer.printCount == 1);
    CHECK(printer.lastJobName == "a.png");
    CHECK(printer.lastWidth == 1);
    CHECK(printer.lastHeight == 1);
    CHECK(nearly(printer.lastOptions.marginMm, 10.0f));
    CHECK(printer.lastOptions.autoRotate);
    CHECK(app.statusBar().leftText == "印刷しました: a.png");

    // 取りやめ: 何も通知しない(保存ダイアログのキャンセルと同じ)
    app.onTimer();  // 直前の通知を消しておく
    printer.status = PrintStatus::Canceled;
    app.execute(Command::PrintImage);
    CHECK(printer.printCount == 2);
    CHECK(app.statusBar().leftText == "1 x 1 px  |  ツール: 矩形");

    // 失敗・未対応はそれぞれの理由を出す
    printer.status = PrintStatus::Failed;
    app.execute(Command::PrintImage);
    CHECK(app.statusBar().leftText == "印刷に失敗しました");
    printer.status = PrintStatus::Unsupported;
    app.execute(Command::PrintImage);
    CHECK(app.statusBar().leftText == "このビルドでは印刷に対応していません");
    printer.status = PrintStatus::Printed;

    // [print] の設定がそのまま渡る
    app.applyConfig(Config::parse("[print]\nmargin_mm = 0\nauto_rotate = false\n"));
    app.execute(Command::PrintImage);
    CHECK(nearly(printer.lastOptions.marginMm, 0.0f));
    CHECK(!printer.lastOptions.autoRotate);

    // 貼り付けた 2x1 の完全に透明な画像を右 90 度回転して印刷する。
    // 表示回転は焼き込まれ (1x2)、紙は白なので透明部分は白で埋まる
    auto pasted = std::make_shared<DecodedImage>();
    pasted->width = 2;
    pasted->height = 1;
    pasted->pixels = {0, 0, 0, 0, 0, 0, 0, 0};
    clipboard.pasteImage = pasted;
    app.execute(Command::PasteImage);
    app.execute(Command::RotateCW);
    app.execute(Command::PrintImage);
    CHECK(printer.lastJobName == "クリップボードの画像");
    CHECK(printer.lastWidth == 1);
    CHECK(printer.lastHeight == 2);
    CHECK(printer.lastPixels.size() == 8);
    CHECK(printer.lastPixels[0] == 255);  // B
    CHECK(printer.lastPixels[3] == 255);  // A(不透明になっている)
}

void testDownscaleToFit() {
    // 4x2。B チャンネルだけ値を変えて箱型フィルタの平均を確かめる
    DecodedImage img;
    img.width = 4;
    img.height = 2;
    const uint8_t blues[8] = {0, 100, 200, 255, 8, 108, 208, 255};
    for (uint8_t b : blues) {
        img.pixels.insert(img.pixels.end(), {b, 255, 255, 255});
    }

    CHECK(downscaleToFit(img, 4) == nullptr);  // 既に収まっているなら何もしない
    CHECK(downscaleToFit(img, 0) == nullptr);
    CHECK(downscaleToFit(DecodedImage{}, 8) == nullptr);  // 空の画像でも落ちない

    // 4x2 → 2x1: 出力 1 画素が入力 2x2 を平均する
    const auto half = downscaleToFit(img, 2);
    CHECK(half != nullptr);
    CHECK(half->width == 2 && half->height == 1);
    CHECK(half->pixels.size() == 8);
    CHECK(half->pixels[0] == (0 + 100 + 8 + 108) / 4);      // 54
    CHECK(half->pixels[4] == (200 + 255 + 208 + 255) / 4);  // 229
    CHECK(half->pixels[1] == 255 && half->pixels[3] == 255);
    // 縮小したことと元の大きさが残る(上書き保存の拒否とステータスバーに使う)
    CHECK(half->downscaled());
    CHECK(half->sourceWidth == 4 && half->sourceHeight == 2);

    // 縦横比は保つ(長い辺を上限に合わせる)
    DecodedImage wide;
    wide.width = 8;
    wide.height = 2;
    wide.pixels.assign(8 * 2 * 4, 128);
    const auto fitted = downscaleToFit(wide, 4);
    CHECK(fitted != nullptr);
    CHECK(fitted->width == 4 && fitted->height == 1);
    CHECK(fitted->pixels == std::vector<uint8_t>(4 * 4, 128));  // 一様な画像は値が変わらない

    // 更に縮めても、記録されている元の大きさは最初のものが残る
    const auto tiny = downscaleToFit(*half, 1);
    CHECK(tiny != nullptr);
    CHECK(tiny->width == 1 && tiny->height == 1);
    CHECK(tiny->sourceWidth == 4 && tiny->sourceHeight == 2);
}

void testAppSidebar() {
    FakeDecoder decoder;
    ImageCache cache(decoder);
    FakeHost host;
    FakeFileSystem fileSystem;
    FakeClipboard clipboard;
    FakeEncoder encoder;
    FakeAnnotationRasterizer rasterizer;
    FakeOcrEngine ocrEngine;
    OcrService ocrService(ocrEngine);
    ScanService scanService(fileSystem);
    FakePrinter printer;
    App app(host, fileSystem, cache, clipboard, encoder, rasterizer, ocrService, printer,
            scanService);
    app.onResize(800, 600);

    // 既定では非表示
    CHECK(!app.sidebar().visible);

    // 30 枚の一覧 (f01..f30) の 10 枚目を開く
    std::mutex mutex;
    std::condition_variable cv;
    bool decoded = false;
    cache.setOnDecoded([&](const std::filesystem::path&) {
        std::lock_guard lock(mutex);
        decoded = true;
        cv.notify_all();
    });
    for (int i = 1; i <= 30; ++i) {
        fileSystem.files.push_back(std::format("C:/pics/f{:02}.png", i));
    }
    app.openPath(fileSystem.files[9]);
    {
        std::unique_lock lock(mutex);
        CHECK(cv.wait_for(lock, std::chrono::seconds(5), [&] { return decoded; }));
    }
    app.onDecodeCompleted();
    CHECK(app.currentImage() != nullptr);

    // Ctrl+B 相当でトグル表示
    app.execute(Command::ToggleSidebar);
    SidebarView sb = app.sidebar();
    CHECK(sb.visible);
    CHECK(nearly(sb.width, 220));
    CHECK(nearly(sb.height, 574));          // 600 - ステータスバー26
    CHECK(nearly(sb.itemHeight, 24));
    CHECK(nearly(sb.contentHeight, 720));   // 30 * 24
    CHECK(nearly(sb.scrollOffset, 0));      // 10 枚目 (y=216..240) は視界内
    CHECK(sb.items.size() == 25);           // 可視範囲のみ (574/24 + 2)
    CHECK(sb.items[0].text == "f01.png");
    CHECK(sb.items[9].current);
    CHECK(!sb.items[0].current);

    // 画像はサイドバーの右側の領域 (580x574) の中央に描画される
    const Point center = app.imageToScreen().apply({0.5f, 0.5f});
    CHECK(nearly(center.x, 220 + 290));
    CHECK(nearly(center.y, 287));

    // 末尾へ移動すると現在項目が見えるまでスクロールする
    app.execute(Command::LastImage);
    sb = app.sidebar();
    CHECK(nearly(sb.scrollOffset, 720 - 574));
    CHECK(sb.items.back().current);

    // サイドバー上のホイールはスクロール (1ノッチ = 3項目) でズームしない
    const float zoomBefore = app.zoom();
    app.onWheel(1.0f, {100, 300});
    sb = app.sidebar();
    CHECK(nearly(sb.scrollOffset, 146 - 72));
    CHECK(nearly(app.zoom(), zoomBefore));
    app.onWheel(-100.0f, {100, 300});  // 大きく下へ → 末尾でクランプ
    CHECK(nearly(app.sidebar().scrollOffset, 146));
    app.onWheel(100.0f, {100, 300});   // 大きく上へ → 先頭でクランプ
    CHECK(nearly(app.sidebar().scrollOffset, 0));

    // ビューポート上のホイールは従来どおりズーム
    app.onWheel(1.0f, {500, 300});
    CHECK(app.zoom() > zoomBefore);
    app.execute(Command::ZoomFit);

    // クリックでジャンプ: scroll=0 で y=100 → index 4 (f05.png)
    CHECK(app.onMouseDown(MouseButton::Left, {100, 100}));
    CHECK(host.lastTitle.find("f05.png") == 0);
    // ビューポート上のクリックは消費しない(パン開始に回す)
    CHECK(!app.onMouseDown(MouseButton::Left, {500, 300}));
    // サイドバー幅内でもステータスバーの高さでは消費のみ(ジャンプしない)
    CHECK(app.onMouseDown(MouseButton::Left, {100, 590}));
    CHECK(host.lastTitle.find("f05.png") == 0);

    // f05 のデコード完了を待って表示を確定させる(以降のチェックを決定的にする)
    {
        std::unique_lock lock(mutex);
        CHECK(cv.wait_for(lock, std::chrono::seconds(5),
                          [&] { return cache.tryGet(fileSystem.files[4]) != nullptr; }));
    }
    app.onDecodeCompleted();
    CHECK(app.currentImage() && app.currentImage()->width == 1);

    // 貼り付け表示中に現在項目をクリック → 一覧表示へ戻る
    auto pasted = std::make_shared<DecodedImage>();
    pasted->width = 2;
    pasted->height = 1;
    pasted->pixels = {0, 0, 0, 255, 0, 0, 0, 255};
    clipboard.pasteImage = pasted;
    app.execute(Command::PasteImage);
    CHECK(host.lastTitle.find("(クリップボード)") == 0);
    CHECK(app.onMouseDown(MouseButton::Left, {100, 100}));  // index 4 = 現在項目
    CHECK(host.lastTitle.find("f05.png") == 0);
    CHECK(app.currentImage() && app.currentImage()->width == 1);

    // フルスクリーン中は非表示
    host.fullscreen = true;
    CHECK(!app.sidebar().visible);
    host.fullscreen = false;

    // ini で初期表示と幅を指定できる
    app.applyConfig(Config::parse("[view]\nsidebar = true\nsidebar_width = 300\n"));
    sb = app.sidebar();
    CHECK(sb.visible);
    CHECK(nearly(sb.width, 300));
}

void testAppSidebarResize() {
    FakeDecoder decoder;
    ImageCache cache(decoder);
    FakeHost host;
    FakeFileSystem fileSystem;
    FakeClipboard clipboard;
    FakeEncoder encoder;
    FakeAnnotationRasterizer rasterizer;
    FakeOcrEngine ocrEngine;
    OcrService ocrService(ocrEngine);
    ScanService scanService(fileSystem);
    FakePrinter printer;
    App app(host, fileSystem, cache, clipboard, encoder, rasterizer, ocrService, printer,
            scanService);
    app.onResize(800, 600);
    std::mutex mutex;
    std::condition_variable cv;
    bool decoded = false;
    cache.setOnDecoded([&](const std::filesystem::path&) {
        std::lock_guard lock(mutex);
        decoded = true;
        cv.notify_all();
    });
    fileSystem.files.push_back("C:/pics/a.png");
    fileSystem.files.push_back("C:/pics/b.png");
    app.openPath(fileSystem.files[0]);
    {
        std::unique_lock lock(mutex);
        CHECK(cv.wait_for(lock, std::chrono::seconds(5), [&] { return decoded; }));
    }
    app.onDecodeCompleted();
    app.execute(Command::ToggleSidebar);
    CHECK(nearly(app.sidebar().width, 220));

    // 右端をまたぐ帯だけが掴める(境界の内側・外側の両方)。項目の上・遠くは掴めない
    CHECK(app.wantsSidebarResizeCursor({218, 300}));
    CHECK(app.wantsSidebarResizeCursor({223, 300}));
    CHECK(!app.wantsSidebarResizeCursor({100, 300}));
    CHECK(!app.wantsSidebarResizeCursor({230, 300}));
    CHECK(!app.wantsSidebarResizeCursor({220, 590}));  // ステータスバーの高さ

    // 右端を掴んでドラッグ → 幅が追従し、項目のジャンプは起きない
    CHECK(app.onMouseDown(MouseButton::Left, {220, 300}));
    CHECK(host.lastTitle.find("a.png") == 0);
    app.onMouseMove({300, 300});
    CHECK(nearly(app.sidebar().width, 300));
    // 画像はサイドバーの右側 (500x574) の中央に描画される
    CHECK(nearly(app.imageToScreen().apply({0.5f, 0.5f}).x, 300 + 250));
    app.onMouseUp(MouseButton::Left, {300, 300}, false);
    app.onMouseMove({400, 300});  // 離した後の移動は幅を変えない
    CHECK(nearly(app.sidebar().width, 300));

    // 下限・上限でクランプし、掴んだ位置からの総移動量で決まるので戻せば追従する
    CHECK(app.onMouseDown(MouseButton::Left, {300, 300}));
    app.onMouseMove({0, 300});
    CHECK(nearly(app.sidebar().width, 120));  // 下限
    app.onMouseMove({790, 300});
    CHECK(nearly(app.sidebar().width, 480));  // 上限
    app.onMouseMove({260, 300});
    CHECK(nearly(app.sidebar().width, 260));  // クランプの後も総移動量に追従する
    app.onMouseUp(MouseButton::Left, {260, 300}, false);

    // 窓が狭ければ上限も狭まる(画像の表示領域を kMinViewportWidth だけ残す)
    app.onResize(400, 600);
    CHECK(app.onMouseDown(MouseButton::Left, {260, 300}));
    app.onMouseMove({600, 300});
    CHECK(nearly(app.sidebar().width, 280));  // 400 - 120
    app.onMouseUp(MouseButton::Left, {600, 300}, false);
    app.onResize(800, 600);

    // 操作一覧モードの下限は kHelpSidebarWidth(それ以上狭めても見た目が変わらないため)
    app.execute(Command::ToggleHelp);
    CHECK(app.sidebarMode() == SidebarMode::Help);
    CHECK(nearly(app.sidebar().width, 300));  // 280 の設定でも操作一覧では 300
    CHECK(app.onMouseDown(MouseButton::Left, {300, 300}));
    app.onMouseMove({100, 300});
    CHECK(nearly(app.sidebar().width, 300));
    app.onMouseMove({420, 300});
    CHECK(nearly(app.sidebar().width, 420));
    app.onMouseUp(MouseButton::Left, {420, 300}, false);

    // 非表示なら掴めない
    app.execute(Command::ToggleHelp);
    CHECK(!app.sidebar().visible);
    CHECK(!app.wantsSidebarResizeCursor({420, 300}));
    CHECK(!app.onMouseDown(MouseButton::Left, {420, 300}));
}

void testAppHelpSidebar() {
    FakeDecoder decoder;
    ImageCache cache(decoder);
    FakeHost host;
    FakeFileSystem fileSystem;
    FakeClipboard clipboard;
    FakeEncoder encoder;
    FakeAnnotationRasterizer rasterizer;
    FakeOcrEngine ocrEngine;
    OcrService ocrService(ocrEngine);
    ScanService scanService(fileSystem);
    FakePrinter printer;
    App app(host, fileSystem, cache, clipboard, encoder, rasterizer, ocrService, printer,
            scanService);
    app.onResize(800, 600);
    for (int i = 1; i <= 30; ++i) {
        fileSystem.files.push_back(std::format("C:/pics/f{:02}.png", i));
    }
    std::mutex mutex;
    std::condition_variable cv;
    bool decoded = false;
    cache.setOnDecoded([&](const std::filesystem::path&) {
        std::lock_guard lock(mutex);
        decoded = true;
        cv.notify_all();
    });
    app.openPath(fileSystem.files[0]);
    {
        std::unique_lock lock(mutex);
        CHECK(cv.wait_for(lock, std::chrono::seconds(5), [&] { return decoded; }));
    }
    app.onDecodeCompleted();
    CHECK(app.currentImage() != nullptr);

    // F1 相当で操作一覧を開く。既定の幅 (220) より広げないと「操作名 + キー」が入らない
    CHECK(app.sidebarMode() == SidebarMode::Files);
    app.execute(Command::ToggleHelp);
    SidebarView sb = app.sidebar();
    CHECK(sb.visible);
    CHECK(app.sidebarMode() == SidebarMode::Help);
    CHECK(nearly(sb.width, 300));
    CHECK(!sb.items.empty());
    CHECK(sb.items[0].text == "表示");
    CHECK(sb.items[0].current);  // 見出しはハイライトで描く
    // 中身はファイル名ではなくキー一覧
    CHECK(sb.items[1].text == "次の画像  Right Down PageDown");
    // 30 ファイルより行数が多いので、内容の高さもファイル一覧とは別物になる
    CHECK(sb.contentHeight > 30 * 24);

    // 画像はサイドバー (300px) の右側に描かれる
    CHECK(nearly(app.imageToScreen().apply({0.5f, 0.5f}).x, 300 + 250));

    // 一覧上のクリックは画像を切り替えない(消費だけする)
    const std::string titleBefore = host.lastTitle;
    CHECK(app.onMouseDown(MouseButton::Left, {100, 100}));
    CHECK(host.lastTitle == titleBefore);

    // ホイールでスクロールでき、末尾・先頭でクランプされる
    app.onWheel(-100.0f, {100, 300});
    const float maxScroll = app.sidebar().contentHeight - app.sidebar().height;
    CHECK(nearly(app.sidebar().scrollOffset, maxScroll));
    app.onWheel(100.0f, {100, 300});
    CHECK(nearly(app.sidebar().scrollOffset, 0));

    // Ctrl+B 相当は閉じずにファイル名一覧へ切り替える
    app.execute(Command::ToggleSidebar);
    sb = app.sidebar();
    CHECK(sb.visible);
    CHECK(app.sidebarMode() == SidebarMode::Files);
    CHECK(nearly(sb.width, 220));
    CHECK(sb.items[0].text == "f01.png");

    // F1 → もう一度 F1 で閉じる
    app.execute(Command::ToggleHelp);
    CHECK(app.sidebar().visible && app.sidebarMode() == SidebarMode::Help);
    app.execute(Command::ToggleHelp);
    CHECK(!app.sidebar().visible);

    // 開いている間の Esc は「閉じる」。アプリ終了やフルスクリーン解除より優先する
    app.execute(Command::ToggleHelp);
    host.fullscreen = true;
    app.execute(Command::Escape);
    CHECK(!app.sidebar().visible);
    CHECK(host.fullscreen);  // フルスクリーンは解除されていない
    app.execute(Command::Escape);
    CHECK(!host.fullscreen);
    host.fullscreen = false;

    // ini でキーを変えると一覧の内容も追従する
    app.applyConfig(Config::parse("[view]\nsidebar_width = 400\n[keys]\nnext = N\n"));
    app.execute(Command::ToggleHelp);
    sb = app.sidebar();
    CHECK(nearly(sb.width, 400));  // 設定が kHelpSidebarWidth より広ければそちらを使う
    CHECK(sb.items[1].text == "次の画像  N");

    // 起動時の案内はステータスバーに出る(未割り当てキーの案内に次ぐ2つ目の導線)
    App fresh(host, fileSystem, cache, clipboard, encoder, rasterizer, ocrService, printer,
            scanService);
    fresh.onResize(800, 600);
    fresh.showStartupHint();
    CHECK(fresh.statusBar().leftText == "F1 で操作一覧");
    CHECK(host.lastTimerMs > 0);  // 一定時間で消える

    // ini で無効にできる
    App quiet(host, fileSystem, cache, clipboard, encoder, rasterizer, ocrService, printer,
            scanService);
    quiet.onResize(800, 600);
    quiet.applyConfig(Config::parse("[view]\nhelp_hint = false\n"));
    quiet.showStartupHint();
    CHECK(quiet.statusBar().leftText.empty());
    CHECK(!quiet.onKey({KeyCode{'J'}}));  // 未割り当てキーでも黙ったまま
    CHECK(quiet.statusBar().leftText.empty());
}

// 未割り当てのキーを押した瞬間 = ヘルプが要る瞬間。ここが案内の主役
void testAppHelpHint() {
    FakeDecoder decoder;
    ImageCache cache(decoder);
    FakeHost host;
    FakeFileSystem fileSystem;
    FakeClipboard clipboard;
    FakeEncoder encoder;
    FakeAnnotationRasterizer rasterizer;
    FakeOcrEngine ocrEngine;
    OcrService ocrService(ocrEngine);
    ScanService scanService(fileSystem);
    FakePrinter printer;
    App app(host, fileSystem, cache, clipboard, encoder, rasterizer, ocrService, printer,
            scanService);
    app.onResize(800, 600);

    // 何も割り当てられていないキー → 消費はしないが案内を出す
    CHECK(!app.onKey({KeyCode{'J'}}));
    CHECK(app.statusBar().leftText == "F1 で操作一覧");

    // 同じ案内の出しっぱなし(キーリピート)では再通知しない
    host.lastTimerMs = 0;
    CHECK(!app.onKey({KeyCode{'J'}}));
    CHECK(host.lastTimerMs == 0);

    // 割り当てのあるキーでは出ない。コマンドの通知があればそちらが優先される
    app.onTimer();  // 案内が時間切れで消えた状態にする
    CHECK(app.statusBar().leftText.empty());
    CHECK(app.onKey({KeyCode{'B'}}));  // ステータスバーの表示切替
    CHECK(app.onKey({KeyCode{'B'}}));  // 元に戻す
    CHECK(app.statusBar().leftText.empty());

    // 操作一覧を出している間は案内しない(見えているものを案内しても意味がない)
    app.execute(Command::ToggleHelp);
    CHECK(!app.onKey({KeyCode{'J'}}));
    CHECK(app.statusBar().leftText.empty());
    app.execute(Command::ToggleHelp);
    CHECK(!app.onKey({KeyCode{'J'}}));
    CHECK(app.statusBar().leftText == "F1 で操作一覧");

    // ini でキーを変えれば案内の文面も変わる
    App rebound(host, fileSystem, cache, clipboard, encoder, rasterizer, ocrService, printer,
            scanService);
    rebound.onResize(800, 600);
    rebound.applyConfig(Config::parse("[keys]\nhelp = H\n"));
    CHECK(!rebound.onKey({KeyCode{'J'}}));
    CHECK(rebound.statusBar().leftText == "H で操作一覧");
    CHECK(rebound.onKey({KeyCode{'H'}}));  // 新しいキーで実際に開く
    CHECK(rebound.sidebarMode() == SidebarMode::Help);

    // ヘルプ自体を無効化(未割り当て)にしたら、案内する先がないので黙る
    App unbound(host, fileSystem, cache, clipboard, encoder, rasterizer, ocrService, printer,
            scanService);
    unbound.onResize(800, 600);
    unbound.applyConfig(Config::parse("[keys]\nhelp =\n"));
    CHECK(!unbound.onKey({KeyCode::F1}));
    CHECK(unbound.statusBar().leftText.empty());
}

void testEditFunctions() {
    // 4x2、B チャンネル = 通し番号*10 の画像
    DecodedImage src;
    src.width = 4;
    src.height = 2;
    src.pixels.resize(4 * 2 * 4);
    for (uint32_t i = 0; i < 8; ++i) {
        const uint8_t v = static_cast<uint8_t>(i * 10);
        src.pixels[i * 4 + 0] = v;
        src.pixels[i * 4 + 1] = static_cast<uint8_t>(v + 1);
        src.pixels[i * 4 + 2] = static_cast<uint8_t>(v + 2);
        src.pixels[i * 4 + 3] = 255;
    }

    // 中央 2x2 の切り出し
    const auto cropped = cropImage(src, {1, 0, 2, 2});
    CHECK(cropped && cropped->width == 2 && cropped->height == 2);
    CHECK(cropped->pixels[0] == 10);      // (0,0) = 元 (1,0)
    CHECK(cropped->pixels[1 * 4] == 20);  // (1,0) = 元 (2,0)
    CHECK(cropped->pixels[2 * 4] == 50);  // (0,1) = 元 (1,1)

    // 画像外へはみ出す指定はクランプされる
    const auto clamped = cropImage(src, {-5, -5, 100, 100});
    CHECK(clamped && clamped->width == 4 && clamped->height == 2);
    CHECK(clamped->pixels == src.pixels);

    // 有効領域が残らなければ nullptr
    CHECK(cropImage(src, {4, 0, 2, 2}) == nullptr);
    CHECK(cropImage(src, {1, 1, 0, 0}) == nullptr);

    // 半透明 (a=128) の事前乗算 over 合成
    DecodedImage dst;
    dst.width = 2;
    dst.height = 2;
    dst.pixels.assign(2 * 2 * 4, 100);
    for (int i = 0; i < 4; ++i) dst.pixels[i * 4 + 3] = 255;
    DecodedImage overlay;
    overlay.width = 1;
    overlay.height = 1;
    overlay.pixels = {0, 0, 128, 128};  // 事前乗算済みの半透明赤
    blendOverlay(dst, overlay, 1, 0);
    CHECK(dst.pixels[1 * 4 + 0] == 50);   // B: 0 + 100*127/255
    CHECK(dst.pixels[1 * 4 + 2] == 178);  // R: 128 + 100*127/255
    CHECK(dst.pixels[1 * 4 + 3] == 255);  // A: 128 + 255*127/255
    CHECK(dst.pixels[0] == 100);          // (0,0) は変化しない

    // 不透明 overlay は上書き。はみ出しはクリップされる
    DecodedImage red;
    red.width = 2;
    red.height = 2;
    red.pixels.resize(2 * 2 * 4);
    for (int i = 0; i < 4; ++i) {
        red.pixels[i * 4 + 2] = 255;
        red.pixels[i * 4 + 3] = 255;
    }
    blendOverlay(dst, red, -1, -1);  // overlay の右下 1 ピクセルだけが (0,0) に載る
    CHECK(dst.pixels[2] == 255);              // (0,0) は赤
    CHECK(dst.pixels[(1 * 2 + 1) * 4] == 100);  // (1,1) は変化しない
}

// 段を見分けるための印として width を使う(画素は要らない)
EditSnapshot snapshotWithWidth(int width) {
    EditSnapshot state;
    state.image = std::make_shared<DecodedImage>();
    state.image->width = width;
    return state;
}

void testEditHistory() {
    EditHistory history;
    CHECK(history.empty() && !history.canUndo() && !history.canRedo());

    // 空の履歴では取り消せない(現在の状態は捨てられ、やり直し側にも積まれない)
    CHECK(!history.undo(snapshotWithWidth(1)).has_value());
    CHECK(!history.redo(snapshotWithWidth(1)).has_value());
    CHECK(history.empty());

    // 1 -> 2 -> 3 と編集した状態を作り、順に戻ってやり直す
    history.push(snapshotWithWidth(1));
    history.push(snapshotWithWidth(2));
    CHECK(history.canUndo() && !history.canRedo() && !history.empty());

    auto back = history.undo(snapshotWithWidth(3));
    CHECK(back && back->image->width == 2);
    CHECK(history.canRedo());
    back = history.undo(snapshotWithWidth(2));
    CHECK(back && back->image->width == 1);
    CHECK(!history.canUndo());  // 使い切ると「開いた直後」に戻る

    auto forward = history.redo(snapshotWithWidth(1));
    CHECK(forward && forward->image->width == 2);
    CHECK(history.canUndo());
    forward = history.redo(snapshotWithWidth(2));
    CHECK(forward && forward->image->width == 3);  // 取り消す前の状態がやり直し先
    CHECK(!history.canRedo());

    // 新しい編集をすると、分岐した未来(やり直し先)は捨てられる
    history.undo(snapshotWithWidth(3));
    CHECK(history.canRedo());
    history.push(snapshotWithWidth(9));
    CHECK(!history.canRedo());

    // 上限を超えた分は古いほうから捨てる
    EditHistory limited;
    for (int i = 0; i < static_cast<int>(EditHistory::kLimit) + 3; ++i) {
        limited.push(snapshotWithWidth(i));
    }
    int steps = 0;
    int oldest = -1;
    while (auto state = limited.undo(snapshotWithWidth(999))) {
        oldest = state->image->width;
        ++steps;
    }
    CHECK(steps == static_cast<int>(EditHistory::kLimit));
    CHECK(oldest == 3);  // 0,1,2 は押し出された

    // ドラッグ中は変更が何度も届くが、積むのは最初の 1 回だけ
    EditHistory drag;
    drag.resetDrag();
    CHECK(!drag.dragPushed());
    CHECK(drag.consumeDragPush());
    CHECK(drag.dragPushed());
    CHECK(!drag.consumeDragPush());  // 2 回目以降は積ませない
    drag.resetDrag();                // 次のドラッグでまた 1 回だけ積める
    CHECK(!drag.dragPushed());
    CHECK(drag.consumeDragPush());

    // テキスト編集も同じ。ただし積むのは開始時に控えた「編集前」の状態
    EditHistory text;
    text.beginTextEdit(snapshotWithWidth(7));
    CHECK(!text.textEditPushed());
    auto before = text.consumeTextEditSnapshot();
    CHECK(before && before->image->width == 7);
    CHECK(text.textEditPushed());
    CHECK(!text.consumeTextEditSnapshot().has_value());

    // clear は履歴も一時状態も落とす(控えた画像を抱え込まない)
    text.push(snapshotWithWidth(1));
    text.undo(snapshotWithWidth(2));
    text.consumeDragPush();
    text.clear();
    CHECK(text.empty() && !text.dragPushed() && !text.textEditPushed());
    CHECK(!text.consumeTextEditSnapshot()->image);  // 控えは空になっている
}

void testSidebarState() {
    SidebarState sb;
    CHECK(!sb.enabled() && sb.mode() == SidebarMode::Files);
    CHECK(nearly(sb.width(), 220) && nearly(sb.scroll(), 0) && !sb.resizing());

    // 同じモードなら開閉のトグル。閉じてもモードは残る(次に開いたときの表示)
    CHECK(sb.toggle(SidebarMode::Files));
    CHECK(sb.showing(SidebarMode::Files) && !sb.showing(SidebarMode::Help));
    CHECK(!sb.toggle(SidebarMode::Files));
    CHECK(!sb.enabled() && sb.mode() == SidebarMode::Files);
    // 別のモードなら閉じずに切り替える
    CHECK(sb.toggle(SidebarMode::Help));
    CHECK(sb.toggle(SidebarMode::Files));
    CHECK(sb.showing(SidebarMode::Files));
    CHECK(sb.toggle(SidebarMode::Help));
    CHECK(!sb.toggle(SidebarMode::Help));
    CHECK(!sb.enabled() && sb.mode() == SidebarMode::Help);  // 閉じても最後のモード

    // 操作一覧モードは狭い設定でも kHelpWidth まで広げて見せる(素の幅は変えない)
    SidebarState widths;
    CHECK(nearly(widths.width(), 220) && nearly(widths.minWidth(), SidebarState::kMinWidth));
    widths.toggle(SidebarMode::Help);
    CHECK(nearly(widths.width(), SidebarState::kHelpWidth));
    CHECK(nearly(widths.configuredWidth(), 220));
    CHECK(nearly(widths.minWidth(), SidebarState::kHelpWidth));
    widths.toggle(SidebarMode::Files);
    CHECK(nearly(widths.width(), 220));

    // 設定 (ini) の幅は上下限だけへ収める(モードもウィンドウ幅も見ない)
    SidebarState configured;
    configured.setConfiguredWidth(1000);
    CHECK(nearly(configured.configuredWidth(), SidebarState::kMaxWidth));
    configured.setConfiguredWidth(0);
    CHECK(nearly(configured.configuredWidth(), SidebarState::kMinWidth));

    SidebarState resized;
    CHECK(resized.setWidth(300, 1000) && nearly(resized.width(), 300));
    CHECK(!resized.setWidth(300, 1000));   // 変わらなければ false(無駄な再描画を避ける)
    CHECK(resized.setWidth(50, 1000));     // 下限
    CHECK(nearly(resized.width(), SidebarState::kMinWidth));
    CHECK(resized.setWidth(999, 1000));    // 上限
    CHECK(nearly(resized.width(), SidebarState::kMaxWidth));
    CHECK(resized.setWidth(999, 400));     // 狭い窓では画像の表示領域を残す
    CHECK(nearly(resized.width(), 400 - SidebarState::kMinViewportWidth));
    resized.toggle(SidebarMode::Help);     // 操作一覧の下限は kHelpWidth
    CHECK(resized.setWidth(100, 1000));
    CHECK(nearly(resized.configuredWidth(), SidebarState::kHelpWidth));

    // スクロールの幾何(1 項目 24px)
    SidebarState scrolled;
    scrolled.scrollToItem(10, 20, 100);
    CHECK(nearly(scrolled.scroll(), 10 * 24 + 24 - 100));  // 下端に入るところまで
    scrolled.scrollToItem(9, 20, 100);                     // 既に見えていれば動かさない
    CHECK(nearly(scrolled.scroll(), 164));
    CHECK(scrolled.firstVisibleItem() == 6);               // 6 番目の途中から見えている
    CHECK(nearly(scrolled.firstItemY(), 6 * 24 - 164));
    CHECK(scrolled.itemAt(0) == 6 && scrolled.itemAt(60) == 9);
    scrolled.scrollToItem(0, 20, 100);                     // 上へ戻すときは項目の上端へ
    CHECK(nearly(scrolled.scroll(), 0));

    scrolled.scrollByItems(3);  // ホイールはクランプしない
    CHECK(nearly(scrolled.scroll(), 72));
    scrolled.clampScroll(20, 100);
    CHECK(nearly(scrolled.scroll(), 72));
    scrolled.clampScroll(3, 100);  // 全部入るなら先頭へ戻す
    CHECK(nearly(scrolled.scroll(), 0));
    scrolled.scrollByItems(-1);
    scrolled.clampScroll(20, 100);
    CHECK(nearly(scrolled.scroll(), 0));

    // 右端を掴む幅変更。帯は境界の内側と外側の両方に広がる
    SidebarState drag;
    CHECK(drag.onResizeEdge(220 - SidebarState::kResizeGripPx));
    CHECK(drag.onResizeEdge(220 + SidebarState::kResizeGripPx));
    CHECK(!drag.onResizeEdge(220 - SidebarState::kResizeGripPx - 1));
    CHECK(!drag.onResizeEdge(220 + SidebarState::kResizeGripPx + 1));
    drag.beginResize(220, drag.width());
    CHECK(drag.resizing());
    // 掴んだ位置からの総移動量で決める(直前の幅からの差分ではない)
    CHECK(nearly(drag.resizeWidth(260), 260));
    CHECK(nearly(drag.resizeWidth(180), 180));
    drag.endResize();
    CHECK(!drag.resizing());
}

void testPointerState() {
    PointerState roles;
    CHECK(roles.role(MouseButton::Left) == MouseRole::Pan);
    CHECK(roles.role(MouseButton::Right) == MouseRole::Edit);
    CHECK(!roles.swapButtons() && !roles.panning() && !roles.inside() && !roles.menuPressed());
    roles.setSwapButtons(true);
    CHECK(roles.role(MouseButton::Left) == MouseRole::Edit);
    CHECK(roles.role(MouseButton::Right) == MouseRole::Pan);

    // 移動量は直前の位置からの差分。ウィンドウ内にいる印も立つ
    PointerState move;
    const Point first = move.moveTo({10, 20});
    CHECK(nearly(first.x, 10) && nearly(first.y, 20) && move.inside());
    const Point second = move.moveTo({13, 18});
    CHECK(nearly(second.x, 3) && nearly(second.y, -2));
    CHECK(nearly(move.lastScreen().x, 13) && nearly(move.lastScreen().y, 18));
    // ドラッグはウィンドウの外で終わりうるので、離した位置は内外を変えない
    move.setInside(false);
    move.setLastScreen({500, 500});
    CHECK(nearly(move.lastScreen().x, 500) && !move.inside());

    PointerState pan;
    pan.beginPan();
    CHECK(pan.panning());
    pan.endPan();
    CHECK(!pan.panning());

    // メニューは押した場所から動かずに離したときだけ出る
    PointerState menu;
    CHECK(menu.releaseMenu({0, 0}) == MenuOnRelease::None);  // 押していない
    menu.pressMenu({100, 100}, false);
    CHECK(menu.menuPressed());
    CHECK(menu.releaseMenu({102, 101}) == MenuOnRelease::Pointer);
    CHECK(!menu.menuPressed());
    CHECK(menu.releaseMenu({102, 101}) == MenuOnRelease::None);  // 押下は消費済み
    menu.pressMenu({100, 100}, true);
    CHECK(menu.releaseMenu({100, 100}) == MenuOnRelease::Sidebar);
    // kDragThresholdPx 動いていればドラッグだったとみなす(パン・編集ドラッグの終わり)
    menu.pressMenu({100, 100}, true);
    CHECK(menu.releaseMenu({100 + PointerState::kDragThresholdPx, 100}) == MenuOnRelease::None);
    // 別の操作が押下を消費したとき(書式メニュー)は通常のメニューを出さない
    menu.pressMenu({100, 100}, false);
    menu.cancelMenu();
    CHECK(menu.releaseMenu({100, 100}) == MenuOnRelease::None);

    // ホイールは 1 段に届くまで貯める。垂直と水平は別の貯金
    PointerState wheel;
    CHECK(wheel.wheelSteps(0.4f, false) == 0);
    CHECK(wheel.wheelSteps(0.4f, true) == 0);
    CHECK(wheel.wheelSteps(0.4f, false) == 0);
    CHECK(wheel.wheelSteps(0.4f, false) == 1);   // 垂直だけ 1.2 に達した
    CHECK(wheel.wheelSteps(0.4f, true) == 0);    // 水平は 0.8 のまま
    CHECK(wheel.wheelSteps(3.0f, false) == 3);   // 一度に何段でも出る
    CHECK(wheel.wheelSteps(-3.0f, false) == 3);  // 向きはコマンド側が決めるので絶対値
    wheel.resetVerticalWheel();
    CHECK(wheel.wheelSteps(0.9f, false) == 0);
    wheel.resetVerticalWheel();
    CHECK(wheel.wheelSteps(0.9f, false) == 0);  // 捨てた分は次に効かない
    wheel.resetHorizontalWheel();
    CHECK(wheel.wheelSteps(0.9f, true) == 0);

    // 水平だけ 1 段の重みを変えられる(トラックボール等の誤爆対策)
    PointerState heavy;
    heavy.setHorizontalThreshold(3.0f);
    CHECK(nearly(heavy.horizontalThreshold(), 3));
    CHECK(heavy.wheelSteps(2.0f, true) == 0);
    CHECK(heavy.wheelSteps(1.0f, true) == 1);
    CHECK(heavy.wheelSteps(1.0f, false) == 1);  // 垂直は 1 ノッチ固定のまま
}

void testAnnotationGeometry() {
    AnnotationSpec rect;
    rect.kind = AnnotationSpec::Kind::Rect;
    rect.p1 = {10, 20};
    rect.p2 = {0, 0};  // 順不同でも正規化される
    rect.strokeWidth = 2;

    const BoundsF b = annotationBounds(rect);
    CHECK(nearly(b.minX, 0) && nearly(b.minY, 0) && nearly(b.maxX, 10) && nearly(b.maxY, 20));
    const Point c = annotationCenter(rect);
    CHECK(nearly(c.x, 5) && nearly(c.y, 10));

    // 矩形: 輪郭の近傍のみヒット(内部は外れてパンに使える)。reach = 太さ/2 + 許容 = 2
    CHECK(hitTestAnnotation(rect, {0, 0}, 1));       // 角
    CHECK(hitTestAnnotation(rect, {-1.5f, 10}, 1));  // 左辺のすぐ外側
    CHECK(!hitTestAnnotation(rect, {5, 10}, 1));     // 中心は外れ
    CHECK(!hitTestAnnotation(rect, {-3, 10}, 1));    // 届かない距離

    // 塗りつぶしてあれば内部もヒットする(塗った領域は見た目どおりに掴める)
    rect.fillAlpha = 128;
    CHECK(hitTestAnnotation(rect, {5, 10}, 1));
    CHECK(!hitTestAnnotation(rect, {-3, 10}, 1));  // 外側は変わらず届かない
    rect.fillAlpha = 0;

    // 90° 回転: 幅10x高20 が中心 (5,10) 周りで横長になる
    rect.angleDeg = 90;
    CHECK(hitTestAnnotation(rect, {c.x + 10, c.y}, 1));   // 回転後の右辺(元の下辺)
    CHECK(!hitTestAnnotation(rect, {c.x, c.y + 10}, 1));  // 回転後の高さは半分の5まで

    // rotatedCorners: 元の TL (0,0) は中心周り 90° 回転で (15,5) へ
    const auto corners = rotatedCorners(rect);
    CHECK(nearly(corners[0].x, 15) && nearly(corners[0].y, 5));
    CHECK(nearly(corners[2].x, -5) && nearly(corners[2].y, 15));  // 元の BR (10,20)

    // 楕円: 輪郭上のみ
    AnnotationSpec ellipse;
    ellipse.kind = AnnotationSpec::Kind::Ellipse;
    ellipse.p1 = {0, 0};
    ellipse.p2 = {20, 10};
    ellipse.strokeWidth = 2;
    CHECK(hitTestAnnotation(ellipse, {20, 5}, 1));   // 右端の輪郭
    CHECK(hitTestAnnotation(ellipse, {10, 0}, 1));   // 上端の輪郭
    CHECK(!hitTestAnnotation(ellipse, {10, 5}, 1));  // 中心は外れ
    ellipse.fillAlpha = 255;
    CHECK(hitTestAnnotation(ellipse, {10, 5}, 1));    // 塗ってあれば内部もヒット
    CHECK(!hitTestAnnotation(ellipse, {0, 0}, 1));    // 楕円の外(bbox の角)は外れたまま
    ellipse.fillAlpha = 0;

    // 直線・矢印: 線分への距離で判定
    AnnotationSpec line;
    line.kind = AnnotationSpec::Kind::Line;
    line.p1 = {0, 0};
    line.p2 = {10, 0};
    line.strokeWidth = 2;
    CHECK(hitTestAnnotation(line, {5, 1.5f}, 1));
    CHECK(!hitTestAnnotation(line, {5, 4}, 1));
    CHECK(!hitTestAnnotation(line, {14, 0}, 1));  // 端点の先

    // テキスト: バウンディングボックス内部
    AnnotationSpec text;
    text.kind = AnnotationSpec::Kind::Text;
    text.p1 = {0, 0};
    text.p2 = {30, 10};
    CHECK(hitTestAnnotation(text, {15, 5}, 1));
    CHECK(!hitTestAnnotation(text, {15, 12}, 1));

    // 重なりは最前面(末尾)が勝つ
    const std::vector<AnnotationSpec> specs{text, line};
    CHECK(hitTestAnnotations(specs, {5, 0.5f}, 1) == std::optional<size_t>(1));
    CHECK(hitTestAnnotations(specs, {25, 8}, 1) == std::optional<size_t>(0));
    CHECK(!hitTestAnnotations(specs, {100, 100}, 1));

    AnnotationSpec moved = line;
    translateAnnotation(moved, 5, 7);
    CHECK(nearly(moved.p1.x, 5) && nearly(moved.p1.y, 7) && nearly(moved.p2.x, 15));

    // Shift ドラッグの正方形化: 一辺は縦横の小さいほうに合わせ、向きは保つ
    CHECK(nearly(constrainToSquare({10, 10}, {40, 30}).x, 30));
    CHECK(nearly(constrainToSquare({10, 10}, {40, 30}).y, 30));
    const Point upLeft = constrainToSquare({10, 10}, {-40, 0});
    CHECK(nearly(upLeft.x, 0) && nearly(upLeft.y, 0));
    const Point degenerate = constrainToSquare({10, 10}, {40, 10});
    CHECK(nearly(degenerate.x, 10) && nearly(degenerate.y, 10));

    // Shift ドラッグの向きスナップ(直線・矢印): 一番近い 8 方向へ寄せる
    const Point horiz = constrainToAxis({10, 10}, {50, 16});  // 8.5 度 → 水平
    CHECK(nearly(horiz.x, 50) && nearly(horiz.y, 10));
    const Point vert = constrainToAxis({10, 10}, {16, -30});  // 81 度 → 垂直
    CHECK(nearly(vert.x, 10) && nearly(vert.y, -30));
    const Point diag = constrainToAxis({10, 10}, {40, 50});  // 53 度 → 45 度
    CHECK(nearly(diag.x, 40) && nearly(diag.y, 40));
    const Point diagUp = constrainToAxis({10, 10}, {-30, -50});  // 左上の 45 度
    CHECK(nearly(diagUp.x, -30) && nearly(diagUp.y, -30));
    // ちょうど真横・真上でも壊れない(始点と同じ点なら動かない)
    CHECK(nearly(constrainToAxis({10, 10}, {40, 10}).y, 10));
    CHECK(nearly(constrainToAxis({10, 10}, {10, 40}).x, 10));
    const Point same = constrainToAxis({10, 10}, {10, 10});
    CHECK(nearly(same.x, 10) && nearly(same.y, 10));

    // 回転ハンドル: 無回転なら上辺中央の真上
    AnnotationSpec plain;
    plain.p1 = {0, 0};
    plain.p2 = {10, 10};
    const Point handle = rotationHandlePos(plain, Matrix3x2::identity(), 20);
    CHECK(nearly(handle.x, 5) && nearly(handle.y, -20));

    CHECK(nearly(angleDegFrom({0, 0}, {10, 0}), 0));
    CHECK(nearly(angleDegFrom({0, 0}, {0, 10}), 90));  // Y 下向きで時計回り
    CHECK(nearly(snapAngleDeg(47, 15), 45));
    CHECK(nearly(snapAngleDeg(83, 15), 90));
    CHECK(nearly(normalizeAngleDeg(-90), 270));
    CHECK(nearly(normalizeAngleDeg(370), 10));

    // サイズ変更ハンドル: 図形は8個、テキストは上下辺なしの6個、直線・矢印は端点2個
    CHECK(resizeHandlePositions(plain).size() == 8);
    CHECK(resizeHandlePositions(text).size() == 6);
    CHECK(resizeHandlePositions(line).size() == 2);

    // 無回転の BR ドラッグは p2 だけ動く。辺ハンドルは一方向のみ
    AnnotationSpec r2 = resizeAnnotation(plain, ResizeHandle::BottomRight, {14, 16}, false);
    CHECK(nearly(r2.p1.x, 0) && nearly(r2.p1.y, 0));
    CHECK(nearly(r2.p2.x, 14) && nearly(r2.p2.y, 16));
    r2 = resizeAnnotation(plain, ResizeHandle::Left, {-4, 100}, false);
    CHECK(nearly(r2.p1.x, -4) && nearly(r2.p1.y, 0));
    CHECK(nearly(r2.p2.x, 10) && nearly(r2.p2.y, 10));

    // 反対側の辺は越えない(最小1px)
    r2 = resizeAnnotation(plain, ResizeHandle::Right, {-100, 5}, false);
    CHECK(nearly(r2.p2.x - r2.p1.x, 1));

    // Shift(縦横比維持)は大きい方の倍率に合わせる
    r2 = resizeAnnotation(plain, ResizeHandle::BottomRight, {20, 15}, true);
    CHECK(nearly(r2.p2.x, 20) && nearly(r2.p2.y, 20));

    // 回転中のリサイズはアンカー(反対側の角)の見た目の位置が変わらない
    AnnotationSpec rot = plain;
    rot.angleDeg = 30;
    const Point tlBefore = rotatedCorners(rot)[0];
    const AnnotationSpec rotResized =
        resizeAnnotation(rot, ResizeHandle::BottomRight, {20, 18}, false);
    CHECK(nearly(rotResized.angleDeg, 30));
    const Point tlAfter = rotatedCorners(rotResized)[0];
    CHECK(nearly(tlAfter.x, tlBefore.x, 0.01f) && nearly(tlAfter.y, tlBefore.y, 0.01f));

    // 端点ドラッグ (Line/Arrow): 他端の見た目の位置は固定される
    AnnotationSpec rline;
    rline.kind = AnnotationSpec::Kind::Line;
    rline.p1 = {0, 0};
    rline.p2 = {10, 0};
    rline.angleDeg = 90;  // 見た目は (5,-5)-(5,5)
    const AnnotationSpec dragged = resizeAnnotation(rline, ResizeHandle::P2, {5, 9}, false);
    CHECK(nearly(dragged.p1.x, -2) && nearly(dragged.p1.y, 2));
    CHECK(nearly(dragged.p2.x, 12) && nearly(dragged.p2.y, 2));
    const auto endpoints = resizeHandlePositions(dragged);
    CHECK(nearly(endpoints[0].pos.x, 5) && nearly(endpoints[0].pos.y, -5));  // P1 は不動
    CHECK(nearly(endpoints[1].pos.x, 5) && nearly(endpoints[1].pos.y, 9));

    // Shift 付きの端点ドラッグ: 回転済みでも見た目が水平・垂直・45 度へ揃う
    const AnnotationSpec snapped = resizeAnnotation(rline, ResizeHandle::P2, {9, 20}, true);
    const auto snappedEnds = resizeHandlePositions(snapped);
    CHECK(nearly(snappedEnds[0].pos.x, 5, 0.01f) && nearly(snappedEnds[0].pos.y, -5, 0.01f));
    CHECK(nearly(snappedEnds[1].pos.x, 5, 0.01f) && nearly(snappedEnds[1].pos.y, 20, 0.01f));
}

void testPastedImageGeometry() {
    // 等倍で収まる大きさなら等倍のまま、中心へ整数座標で置かれる
    BoundsF b = pastedImageBounds({40, 20}, {200, 100}, {100, 50});
    CHECK(nearly(b.minX, 80) && nearly(b.minY, 40));
    CHECK(nearly(b.maxX, 120) && nearly(b.maxY, 60));

    // 下地に対して大きすぎる画像は、縦横比を保って 80% へ収まるまで縮む
    b = pastedImageBounds({400, 200}, {200, 100}, {100, 50});
    CHECK(nearly(b.maxX - b.minX, 160) && nearly(b.maxY - b.minY, 80));

    // 中心が端に寄っていても全体が下地へ収まる
    b = pastedImageBounds({40, 20}, {200, 100}, {0, 0});
    CHECK(nearly(b.minX, 0) && nearly(b.minY, 0));
    b = pastedImageBounds({40, 20}, {200, 100}, {1000, 1000});
    CHECK(nearly(b.maxX, 200) && nearly(b.maxY, 100));

    // 大きさが不正なら空の矩形(呼び出し側で貼らない判断ができる)
    b = pastedImageBounds({0, 20}, {200, 100}, {100, 50});
    CHECK(nearly(b.maxX - b.minX, 0) && nearly(b.maxY - b.minY, 0));

    // 画像オブジェクトは既定で縦横比を維持し、Shift で解除する(他の種別とは逆)
    AnnotationSpec image;
    image.kind = AnnotationSpec::Kind::Image;
    CHECK(resizeKeepsAspect(image, false));
    CHECK(!resizeKeepsAspect(image, true));
    const AnnotationSpec rect;  // 既定は Rect
    CHECK(!resizeKeepsAspect(rect, false));
    CHECK(resizeKeepsAspect(rect, true));

    // ヒットテストは箱の内部全体(テキストと同じ)、ハンドルは四隅だけ
    image.p1 = {10, 10};
    image.p2 = {50, 30};
    CHECK(hitTestAnnotation(image, {30, 20}, 1));
    CHECK(!hitTestAnnotation(image, {60, 20}, 1));
    CHECK(resizeHandlePositions(image).size() == 4);

    // 四隅ドラッグは比率を保つ(大きい側の倍率に合わせる)
    const AnnotationSpec resized =
        resizeAnnotation(image, ResizeHandle::BottomRight, {90, 35}, true);
    CHECK(nearly(resized.p2.x - resized.p1.x, 80));
    CHECK(nearly(resized.p2.y - resized.p1.y, 40));
}

void testPenGeometry() {
    // 点の間引き: 直前の点から minDistance 未満なら捨てる(最初の1点は必ず入る)
    std::vector<Point> points;
    CHECK(appendPenPoint(points, {0, 0}, 2));
    CHECK(!appendPenPoint(points, {1, 0}, 2));  // 近すぎる
    CHECK(appendPenPoint(points, {2, 0}, 2));   // ちょうど 2 は通す
    CHECK(appendPenPoint(points, {2, 0}, 0));   // minDistance 0 なら重複でも通す
    CHECK(points.size() == 3);

    AnnotationSpec pen;
    pen.kind = AnnotationSpec::Kind::Pen;
    pen.strokeWidth = 2;
    pen.points = {{0, 0}, {20, 0}, {20, 20}};  // 右へ引いてから下へ曲げた L 字
    pen.p1 = {99, 99};                         // updatePenBounds が上書きする
    pen.p2 = {99, 99};
    updatePenBounds(pen);
    CHECK(nearly(pen.p1.x, 0) && nearly(pen.p1.y, 0));
    CHECK(nearly(pen.p2.x, 20) && nearly(pen.p2.y, 20));

    // 当たるのは線の上だけ。bbox の内側でも線から離れていれば外れる
    CHECK(hitTestAnnotation(pen, {10, 1}, 1));    // 横棒の上
    CHECK(hitTestAnnotation(pen, {20, 15}, 1));   // 縦棒の上
    CHECK(!hitTestAnnotation(pen, {10, 10}, 1));  // L の内側(bbox 内)
    CHECK(!hitTestAnnotation(pen, {25, 25}, 1));  // bbox の外

    // 1 点だけのストローク(点を打っただけ)は点への距離で判定する
    AnnotationSpec dot = pen;
    dot.points = {{5, 5}};
    updatePenBounds(dot);
    CHECK(hitTestAnnotation(dot, {5, 6}, 1));
    CHECK(!hitTestAnnotation(dot, {5, 9}, 1));

    // 平行移動は点列も一緒に動く(bbox との同期が崩れない)
    AnnotationSpec movedPen = pen;
    translateAnnotation(movedPen, 5, -3);
    CHECK(nearly(movedPen.points[1].x, 25) && nearly(movedPen.points[1].y, -3));
    CHECK(nearly(movedPen.p1.x, 5) && nearly(movedPen.p1.y, -3));
    CHECK(hitTestAnnotation(movedPen, {15, -2}, 1));

    // リサイズは点列を bbox と同じ比率で写す(線幅は変えない)
    const AnnotationSpec scaled =
        resizeAnnotation(pen, ResizeHandle::BottomRight, {40, 10}, false);
    CHECK(nearly(scaled.p2.x, 40) && nearly(scaled.p2.y, 10));
    CHECK(scaled.points.size() == 3);
    CHECK(nearly(scaled.points[1].x, 40) && nearly(scaled.points[1].y, 0));
    CHECK(nearly(scaled.points[2].x, 40) && nearly(scaled.points[2].y, 10));
    CHECK(nearly(scaled.strokeWidth, 2));

    // 連番マーカー: 中身の詰まった円なので内部も当たり、円の外は外れる
    AnnotationSpec number;
    number.kind = AnnotationSpec::Kind::Number;
    number.p1 = {0, 0};
    number.p2 = {20, 20};
    number.number = 3;
    CHECK(hitTestAnnotation(number, {10, 10}, 1));  // 中心
    CHECK(hitTestAnnotation(number, {10, 0}, 1));   // 上端
    CHECK(!hitTestAnnotation(number, {1, 1}, 1));   // bbox の角(円の外)
    CHECK(nearly(numberFontSize(number), 12));      // 直径の 60%

    // 円を保つため、ハンドルは四隅だけで、辺を掴めない(= 楕円にできない)
    CHECK(resizeHandlePositions(number).size() == 4);
    // Shift なしでも縦横比が維持される
    const AnnotationSpec resizedNumber =
        resizeAnnotation(number, ResizeHandle::BottomRight, {40, 25}, false);
    CHECK(nearly(resizedNumber.p2.x - resizedNumber.p1.x,
                 resizedNumber.p2.y - resizedNumber.p1.y));
    CHECK(nearly(resizedNumber.p2.x, 40));
}

void testAppAnnotationObjects() {
    FakeDecoder decoder;
    ImageCache cache(decoder);
    FakeHost host;
    FakeFileSystem fileSystem;
    FakeClipboard clipboard;
    FakeEncoder encoder;
    FakeAnnotationRasterizer rasterizer;
    FakeOcrEngine ocrEngine;
    OcrService ocrService(ocrEngine);
    ScanService scanService(fileSystem);
    FakePrinter printer;
    App app(host, fileSystem, cache, clipboard, encoder, rasterizer, ocrService, printer,
            scanService);
    app.onResize(800, 600);

    // 8x8 画像を貼り付け、矩形注釈 (0,0)-(4,4) を追加(画像左上はスクリーン (396,283))
    auto source = std::make_shared<DecodedImage>();
    source->width = 8;
    source->height = 8;
    source->pixels.resize(8 * 8 * 4);
    clipboard.pasteImage = source;
    app.execute(Command::PasteImage);
    CHECK(app.currentTool() == EditTool::Rect);  // 既定のツールは矩形
    app.onMouseDown(MouseButton::Right, {396, 283});
    app.onMouseUp(MouseButton::Right, {400, 287});
    CHECK(host.menuCount == 0);  // ツールは決まっているのでメニューは出ない
    CHECK(app.annotations().specs->size() == 1);
    CHECK(app.annotations().selected.has_value());

    // Escape はまず選択解除に使われる
    app.execute(Command::Escape);
    CHECK(!app.annotations().selected.has_value());

    // 注釈の輪郭をクリックすると選択して移動ドラッグが始まる(クリックを消費しパンしない)
    CHECK(app.onMouseDown(MouseButton::Left, {396, 283}));  // 画像 (0,0) = 矩形の角
    CHECK(app.annotations().selected == std::optional<size_t>(0));
    app.onMouseMove({398, 285});  // +2px 移動
    {
        const AnnotationsView view = app.annotations();
        const AnnotationSpec& spec = view.specs->front();
        CHECK(nearly(spec.p1.x, 2) && nearly(spec.p1.y, 2));
        CHECK(nearly(spec.p2.x, 6) && nearly(spec.p2.y, 6));
    }
    app.onMouseMove({397, 284});  // ドラッグ継続(+1px に戻す)
    CHECK(nearly(app.annotations().specs->front().p1.x, 1));
    app.onMouseUp(MouseButton::Left);

    // ドラッグ1回の undo は1段。取り消しで元の位置に戻り選択は解除される
    app.execute(Command::Undo);
    CHECK(nearly(app.annotations().specs->front().p1.x, 0));
    CHECK(!app.annotations().selected.has_value());

    // 何もない場所のクリックは消費しない(選択解除してパンに回る)
    CHECK(app.onMouseDown(MouseButton::Left, {396, 283}));
    app.onMouseUp(MouseButton::Left);
    CHECK(!app.onMouseDown(MouseButton::Left, {600, 450}));
    CHECK(!app.annotations().selected.has_value());
    app.onMouseUp(MouseButton::Left);

    // サイズ変更: 選択中の右下ハンドル(画像 (4,4) = スクリーン (400,287))をドラッグ
    CHECK(app.onMouseDown(MouseButton::Left, {396, 283}));  // まず本体クリックで選択
    app.onMouseUp(MouseButton::Left);
    CHECK(app.onMouseDown(MouseButton::Left, {400, 287}));  // 右下ハンドルを掴む
    app.onMouseMove({402, 289});
    {
        const AnnotationsView view = app.annotations();
        const AnnotationSpec& spec = view.specs->front();
        CHECK(nearly(spec.p1.x, 0) && nearly(spec.p1.y, 0));
        CHECK(nearly(spec.p2.x, 6) && nearly(spec.p2.y, 6));
    }
    app.onMouseUp(MouseButton::Left);
    app.execute(Command::Undo);  // リサイズ1回で undo 1段
    CHECK(nearly(app.annotations().specs->front().p2.x, 4));
    CHECK(!app.annotations().selected.has_value());

    // 回転ハンドル(枠上辺中央の 20px 上)のドラッグで回転する
    CHECK(app.onMouseDown(MouseButton::Left, {396, 283}));  // 選択し直す
    app.onMouseUp(MouseButton::Left);
    CHECK(app.onMouseDown(MouseButton::Left, {398, 263}));  // 中心 (398,285)、ハンドル (398,263)
    app.onMouseMove({420, 285});         // 中心の真右 → 90°
    CHECK(nearly(app.annotations().specs->front().angleDeg, 90));
    app.onMouseMove({421, 287}, true);   // Shift で 15° 単位にスナップ
    CHECK(nearly(std::fmod(app.annotations().specs->front().angleDeg, 15.0f), 0));
    app.onMouseUp(MouseButton::Left);
    app.execute(Command::Undo);
    CHECK(nearly(app.annotations().specs->front().angleDeg, 0));

    // 右クリック(ドラッグ閾値未満)でオブジェクトメニュー。末端 index (図形):
    // 0 削除, 1-8 回転 {0,15,30,45,90,135,180,270}, 9-15 太さ {1,2,3,5,8,12,20}, 16 色,
    // 17-21 塗りつぶし {0,64,128,191,255}, 22 塗りつぶしの色
    host.menuChoice = 5;  // 90°
    app.onMouseDown(MouseButton::Right, {396, 283});
    app.onMouseUp(MouseButton::Right, {397, 283});
    CHECK(countMenuLeaves(host.lastMenuItems) == 23);
    CHECK(nearly(app.annotations().specs->front().angleDeg, 90));

    host.menuChoice = 13;  // 太さ 8px
    app.onMouseDown(MouseButton::Right, {396, 283});
    app.onMouseUp(MouseButton::Right, {396, 283});
    CHECK(nearly(app.annotations().specs->front().strokeWidth, 8));

    host.colorChoice = 0x123456;
    host.menuChoice = 16;  // 色の変更
    app.onMouseDown(MouseButton::Right, {396, 283});
    app.onMouseUp(MouseButton::Right, {396, 283});
    CHECK(app.annotations().specs->front().colorRGB == 0x123456);

    // テキスト注釈はその場で入力して追加し、ダブルクリックで再編集する
    chooseInToolMenu(app, host, {9});  // テキスト
    CHECK(app.currentTool() == EditTool::Text);
    rasterizer.overlayWidth = 24;
    rasterizer.overlayHeight = 44;  // 実測境界 20x40(リサイズテストでハンドルを離すため縦長)
    const int measureCount = rasterizer.rasterizeCount;
    app.onMouseDown(MouseButton::Right, {401, 286});  // 画像 (5,3)
    app.onMouseUp(MouseButton::Right, {404, 290});    // 閾値以上のドラッグでテキストボックスができる
    CHECK(app.isTextEditing());        // 空のテキストボックスができ、その場で入力できる
    CHECK(host.textEditing);           // host には編集開始が伝わる (IME 有効化)
    CHECK(rasterizer.rasterizeCount == measureCount);  // 内容が空の間は実測しない
    app.insertText("元のテキスト");
    app.onKey({KeyCode::Escape});      // Esc で確定
    CHECK(!app.isTextEditing());
    CHECK(!host.textEditing);
    CHECK(rasterizer.rasterizeCount == measureCount + 2);  // 高さ合わせ + 確定時の実測
    CHECK(app.annotations().specs->size() == 2);
    CHECK(app.annotations().specs->back().text == "元のテキスト");

    CHECK(app.onDoubleClick({402, 288}));  // テキスト上のダブルクリックで再編集を始める
    CHECK(app.isTextEditing());
    app.insertText("更新後");  // ダブルクリックで語が選択されているため置き換わる
    app.onKey({KeyCode::Escape});
    CHECK(app.annotations().specs->back().text == "更新後");
    CHECK(!app.onDoubleClick({600, 450}));  // テキスト以外の場所では何もしない

    // Delete で選択中の注釈を削除。選択なしはメッセージのみ。undo で復活する
    app.execute(Command::DeleteAnnotation);  // ダブルクリックで選択済み
    CHECK(app.annotations().specs->size() == 1);
    app.execute(Command::DeleteAnnotation);
    CHECK(app.statusBar().leftText == "削除する注釈がありません");
    app.onTimer();
    app.execute(Command::Undo);
    CHECK(app.annotations().specs->size() == 2);
    CHECK(app.annotations().specs->back().text == "更新後");

    // トリミング後も注釈はオブジェクトのまま維持され、座標が平行移動する
    chooseInToolMenu(app, host, {0});  // トリミング
    CHECK(app.currentTool() == EditTool::Crop);
    app.onMouseDown(MouseButton::Right, {398, 285});  // 画像 (2,2)
    app.onMouseUp(MouseButton::Right, {402, 289});    // 画像 (6,6) → 4x4 に切り出し
    CHECK(app.currentImage()->width == 4);
    CHECK(app.annotations().specs->size() == 2);
    CHECK(nearly(app.annotations().specs->front().p1.x, -2));  // (0,0) → (-2,-2)
    // トリミングは一度きり。実行すると直前に使っていた図形ツール(テキスト)へ戻る
    CHECK(app.currentTool() == EditTool::Text);
    app.execute(Command::Undo);
    CHECK(app.currentImage()->width == 8);
    CHECK(nearly(app.annotations().specs->front().p1.x, 0));

    // 保存は注釈を合成した画像を出力する(注釈の数だけラスタライズされる)
    const int rasterizeBeforeSave = rasterizer.rasterizeCount;
    host.savePath = std::filesystem::path("C:/out/annotated.png");
    app.execute(Command::SaveImageAs);
    CHECK(encoder.lastWidth == 8);
    CHECK(rasterizer.rasterizeCount == rasterizeBeforeSave + 2);

    // テキストのリサイズ: 右ハンドルで折り返し幅を変え、確定時に実寸へ揃える。
    // テキストは (5,3)-(25,43)、右ハンドルは画像 (25,23) = スクリーン (421,306)
    CHECK(app.onMouseDown(MouseButton::Left, {402, 288}));  // 本体クリックで選択(最前面のテキスト)
    app.onMouseUp(MouseButton::Left);
    CHECK(app.annotations().selected == std::optional<size_t>(1));
    CHECK(app.onMouseDown(MouseButton::Left, {421, 306}));
    const int beforeResize = rasterizer.rasterizeCount;
    app.onMouseMove({431, 306});
    CHECK(nearly(app.annotations().specs->back().p2.x, 35));  // ドラッグ中は掴んだ幅のまま
    CHECK(rasterizer.rasterizeCount == beforeResize);
    app.onMouseUp(MouseButton::Left);  // 確定時に折り返し後の実寸を測り直す
    CHECK(rasterizer.rasterizeCount == beforeResize + 1);
    CHECK(nearly(app.annotations().specs->back().p2.x, 25));
    CHECK(nearly(app.annotations().specs->back().p2.y, 43));
}

void testAppEdit() {
    FakeDecoder decoder;
    ImageCache cache(decoder);
    FakeHost host;
    FakeFileSystem fileSystem;
    FakeClipboard clipboard;
    FakeEncoder encoder;
    FakeAnnotationRasterizer rasterizer;
    FakeOcrEngine ocrEngine;
    OcrService ocrService(ocrEngine);
    ScanService scanService(fileSystem);
    FakePrinter printer;
    App app(host, fileSystem, cache, clipboard, encoder, rasterizer, ocrService, printer,
            scanService);
    app.onResize(800, 600);

    // 画像がないときは選択を開始しない
    app.onMouseDown(MouseButton::Right, {400, 300});
    CHECK(!app.selection().visible);
    app.onMouseUp(MouseButton::Right, {500, 400});
    CHECK(host.menuCount == 0);

    // 8x8 の不透明青画像を貼り付けて編集対象にする。
    // ビューポート 800x574 の中央に等倍表示され、画像左上はスクリーン (396, 283)
    auto source = std::make_shared<DecodedImage>();
    source->width = 8;
    source->height = 8;
    source->pixels.resize(8 * 8 * 4);
    for (size_t i = 0; i < source->pixels.size(); i += 4) {
        source->pixels[i] = 255;      // B
        source->pixels[i + 3] = 255;  // A
    }
    clipboard.pasteImage = source;
    app.execute(Command::PasteImage);
    CHECK(app.currentImage() && app.currentImage()->width == 8);

    // 起動時のツールは ini で指定できる。未知の名前なら既定(矩形)のまま
    app.applyConfig(Config::parse("[edit]\ntool = Arrow\n"));
    CHECK(app.currentTool() == EditTool::Arrow);
    app.applyConfig(Config::parse("[edit]\ntool = nonsense\n"));
    CHECK(app.currentTool() == EditTool::Arrow);  // 解釈できない指定は無視する
    app.applyConfig(Config::parse("[edit]\ntool = rect\n"));
    CHECK(app.currentTool() == EditTool::Rect);

    // コマンドからもツールを切り替えられる(既定のキーは無く ini で割り当てる)
    app.execute(Command::SelectToolArrow);
    CHECK(app.currentTool() == EditTool::Arrow);
    CHECK(app.statusBar().leftText == "8 x 8 px  |  ツール: 矢印");
    app.execute(Command::SelectToolRect);

    // 既定のツールは矩形。ドラッグ中はラバーバンドではなく実物のプレビューが出る
    CHECK(app.currentTool() == EditTool::Rect);
    app.onMouseDown(MouseButton::Right, {396, 283});
    app.onMouseMove({400, 287});
    {
        const AnnotationsView view = app.annotations();
        CHECK(view.preview != nullptr);
        CHECK(view.preview->kind == AnnotationSpec::Kind::Rect);
        CHECK(nearly(view.preview->p1.x, 0) && nearly(view.preview->p2.x, 4));
        CHECK(nearly(view.preview->strokeWidth, 3));  // 確定後と同じ設定で描かれる
        CHECK(view.specs->empty());                   // まだ確定していない
        CHECK(!app.selection().visible);              // プレビュー中はラバーバンドを出さない
    }
    app.execute(Command::Escape);  // Escape で解除できる
    CHECK(app.annotations().preview == nullptr);
    CHECK(app.annotations().specs->empty());

    // Shift ドラッグは正方形になる。画像 (0,0)-(6,5) → 短いほう(5)に合わせて (5,5)
    app.onMouseDown(MouseButton::Right, {396, 283});
    app.onMouseMove({402, 288}, true);
    CHECK(nearly(app.annotations().preview->p2.x, 5));
    CHECK(nearly(app.annotations().preview->p2.y, 5));
    app.onMouseUp(MouseButton::Right, {402, 288}, true);
    CHECK(app.annotations().specs->size() == 1);
    {
        const AnnotationSpec spec = app.annotations().specs->back();
        CHECK(nearly(spec.p1.x, 0) && nearly(spec.p1.y, 0));
        CHECK(nearly(spec.p2.x, 5) && nearly(spec.p2.y, 5));
    }

    // 真円も同じ経路。左上向きのドラッグでも符号を保つ(画像 (6,6) → (1,1))
    app.execute(Command::SelectToolEllipse);
    app.onMouseDown(MouseButton::Right, {402, 289});
    app.onMouseUp(MouseButton::Right, {397, 284}, true);
    {
        const AnnotationSpec spec = app.annotations().specs->back();
        CHECK(spec.kind == AnnotationSpec::Kind::Ellipse);
        CHECK(nearly(spec.p1.x, 6) && nearly(spec.p1.y, 6));
        CHECK(nearly(spec.p2.x, 1) && nearly(spec.p2.y, 1));
    }

    // 直線・矢印は正方形化(=45 度固定)ではなく、向きを水平・垂直・45 度へ寄せる。
    // 画像 (0,0)-(7,2) は水平寄りなので (7,0) までの真横の線になる
    app.execute(Command::SelectToolLine);
    app.onMouseDown(MouseButton::Right, {396, 283});
    app.onMouseUp(MouseButton::Right, {403, 285}, true);
    {
        const AnnotationSpec spec = app.annotations().specs->back();
        CHECK(spec.kind == AnnotationSpec::Kind::Line);
        CHECK(nearly(spec.p2.x, 7) && nearly(spec.p2.y, 0));
    }

    // 縦寄りなら垂直。ドラッグ中のプレビューも同じ位置に寄る
    app.execute(Command::SelectToolArrow);
    app.onMouseDown(MouseButton::Right, {396, 283});
    app.onMouseMove({398, 290}, true);
    CHECK(nearly(app.annotations().preview->p2.x, 0));
    CHECK(nearly(app.annotations().preview->p2.y, 7));
    // ドラッグ中に Shift を離せばプレビューは素通しへ戻る
    CHECK(app.onShiftChanged(false));
    CHECK(nearly(app.annotations().preview->p2.x, 2));
    app.onMouseUp(MouseButton::Right, {398, 290}, true);
    {
        const AnnotationSpec spec = app.annotations().specs->back();
        CHECK(spec.kind == AnnotationSpec::Kind::Arrow);
        CHECK(nearly(spec.p2.x, 0) && nearly(spec.p2.y, 7));
    }

    // どちらでもない向きは 45 度。画像 (0,0)-(6,5) → 短いほう(5)に合わせて (5,5)
    app.execute(Command::SelectToolLine);
    app.onMouseDown(MouseButton::Right, {396, 283});
    app.onMouseUp(MouseButton::Right, {402, 288}, true);
    {
        const AnnotationSpec spec = app.annotations().specs->back();
        CHECK(nearly(spec.p2.x, 5) && nearly(spec.p2.y, 5));
    }

    // Shift なしはそのまま(スナップは Shift のときだけ)
    app.onMouseDown(MouseButton::Right, {396, 283});
    app.onMouseUp(MouseButton::Right, {403, 285}, false);
    {
        const AnnotationSpec spec = app.annotations().specs->back();
        CHECK(nearly(spec.p2.x, 7) && nearly(spec.p2.y, 2));
    }

    app.execute(Command::SelectToolRect);
    while (!app.annotations().specs->empty()) app.execute(Command::Undo);
    CHECK(app.annotations().specs->empty());

    // 閾値未満の右ドラッグ(ただの右クリック)はツール切り替えメニューを開く。
    // 末端項目: ツール10種 + 太さ7 + 文字サイズ7 + フォント9 + 色1 + 塗りつぶし(5+色1)
    // + 枠線(6+色1) + リサイズ(倍率5+長辺7) = 59(回転角度はオブジェクト側にある)
    host.menuChoice = std::nullopt;  // キャンセルするので何も起きない
    app.onMouseDown(MouseButton::Right, {400, 300});
    app.onMouseUp(MouseButton::Right, {402, 301});
    CHECK(host.menuCount == 1);
    CHECK(countMenuLeaves(host.lastMenuItems) == 59);
    CHECK(app.currentTool() == EditTool::Rect);
    CHECK(app.annotations().specs->empty());
    CHECK(app.currentImage()->width == 8);
    CHECK(rasterizer.rasterizeCount == 0);

    // トリミングとテキストは形が定まらないのでラバーバンドを出す
    chooseInToolMenu(app, host, {0});  // トリミング
    CHECK(app.currentTool() == EditTool::Crop);
    app.onMouseDown(MouseButton::Right, {396, 283});
    app.onMouseMove({400, 287});
    {
        const SelectionView sel = app.selection();
        CHECK(sel.visible);
        CHECK(nearly(sel.p1.x, 396) && nearly(sel.p1.y, 283));
        CHECK(nearly(sel.p2.x, 400) && nearly(sel.p2.y, 287));
        CHECK(app.annotations().preview == nullptr);
    }

    // トリミング: 画像座標 (0,0)-(4,4) → 4x4。一度きりの操作なので実行後は矩形へ戻る
    app.onMouseUp(MouseButton::Right, {400, 287});
    CHECK(app.currentImage()->width == 4 && app.currentImage()->height == 4);
    CHECK(app.currentTool() == EditTool::Rect);
    CHECK(app.statusBar().leftText == "4 x 4 px  |  ツール: 矩形");
    CHECK(host.lastTitle.find("(編集済み)") != std::string::npos);
    CHECK(!app.selection().visible);

    // Undo で元に戻る。履歴が空ならメッセージ
    app.execute(Command::Undo);
    CHECK(app.currentImage()->width == 8);
    CHECK(host.lastTitle.find("(編集済み)") == std::string::npos);
    app.execute(Command::Undo);
    CHECK(app.statusBar().leftText == "取り消す編集はありません");
    app.onTimer();

    // 矩形: 画像へは焼き込まず注釈オブジェクトとして追加され、追加直後は選択状態になる
    app.onMouseDown(MouseButton::Right, {396, 283});
    app.onMouseUp(MouseButton::Right, {400, 287});
    CHECK(rasterizer.rasterizeCount == 0);  // 図形の追加ではラスタライズしない
    {
        const AnnotationsView view = app.annotations();
        CHECK(view.specs && view.specs->size() == 1);
        const AnnotationSpec& spec = view.specs->front();
        CHECK(spec.kind == AnnotationSpec::Kind::Rect);
        CHECK(nearly(spec.p1.x, 0) && nearly(spec.p2.x, 4));
        CHECK(nearly(spec.strokeWidth, 3));  // 既定の太さ(画像px)
        CHECK(spec.colorRGB == 0xFF3B30);
        CHECK(nearly(spec.angleDeg, 0));
        CHECK(view.selected && *view.selected == 0);
    }
    CHECK(host.lastTitle.find("(編集済み)") != std::string::npos);
    CHECK(app.currentImage()->pixels[(1 * 8 + 1) * 4 + 2] == 0);  // 画像自体は無変更

    // コピーは注釈を合成した画像になる (2x2 赤 overlay が (1,1) へ)。
    // 選択中はオブジェクトだけがコピーされるので、まず選択を外す
    // (オブジェクト側の経路は testAppPasteObject で見る)
    rasterizer.overlayWidth = 2;
    rasterizer.overlayHeight = 2;
    rasterizer.overlayX = 1;
    rasterizer.overlayY = 1;
    app.execute(Command::Escape);
    CHECK(!app.annotations().selected.has_value());
    app.execute(Command::CopyImage);
    CHECK(rasterizer.rasterizeCount == 1);
    CHECK(clipboard.lastWidth == 8);
    CHECK(clipboard.lastPixels[(1 * 8 + 1) * 4 + 2] == 255);  // (1,1) は赤
    CHECK(clipboard.lastPixels[2] == 0);                      // (0,0) は青のまま
    CHECK(app.currentImage()->pixels[(1 * 8 + 1) * 4 + 2] == 0);  // 元画像は無変更
    // 貼り付け元(キャッシュ相当)の画像も書き換えられていない
    CHECK(source->pixels[(1 * 8 + 1) * 4 + 2] == 0);

    // テキスト: 空のまま確定すると追加されない。入力があれば実測して追加される
    chooseInToolMenu(app, host, {9});
    app.onMouseDown(MouseButton::Right, {396, 283});
    app.onMouseUp(MouseButton::Right, {400, 287});
    CHECK(app.isTextEditing());
    CHECK(app.annotations().specs->size() == 2);  // 編集中は空のテキストボックスが入る
    app.onKey({KeyCode::Escape});
    CHECK(rasterizer.rasterizeCount == 1);        // 空なので実測は走らない
    CHECK(app.annotations().specs->size() == 1);  // 空の箱は残さない
    rasterizer.overlayWidth = 24;   // 実測境界: 24-4 x 12-4 (テキスト余白 2px x 両側)
    rasterizer.overlayHeight = 12;
    app.onMouseDown(MouseButton::Right, {396, 283});
    app.onMouseUp(MouseButton::Right, {400, 287});
    app.insertText("メモ");
    app.onKey({KeyCode::Escape});
    CHECK(rasterizer.rasterizeCount == 3);  // 高さ合わせ + 確定時の実測
    CHECK(rasterizer.lastSpec.kind == AnnotationSpec::Kind::Text);
    CHECK(rasterizer.lastSpec.text == "メモ");
    {
        const AnnotationsView view = app.annotations();
        const AnnotationSpec& text = view.specs->back();
        CHECK(nearly(text.fontSize, 18));  // 既定値(画像px)
        CHECK(nearly(text.p1.x, 0) && nearly(text.p1.y, 0));
        CHECK(nearly(text.p2.x, 20) && nearly(text.p2.y, 8));  // 実測境界が p2 に入る
    }

    // 設定変更を選ぶとメニューが再表示され、続けてツールを選べる。
    // 末端 index: 0-5 ツール, 6-12 太さ {1,2,3,5,8,12,20}, 13-19 文字サイズ
    // {12,14,18,24,36,48,72}, 20-27 フォント, 28 色
    chooseInToolMenu(app, host, {14 /*太さ8px*/, 2 /*矩形*/});
    CHECK(host.menuQueue.empty());
    app.onMouseDown(MouseButton::Right, {396, 283});
    app.onMouseUp(MouseButton::Right, {400, 287});
    CHECK(nearly(app.annotations().specs->back().strokeWidth, 8));

    // 文字サイズ 24px + 複数行テキスト。変更済みの設定も引き継がれる
    chooseInToolMenu(app, host, {20 /*文字24px*/, 9 /*テキスト*/});
    app.onMouseDown(MouseButton::Right, {396, 283});
    app.onMouseUp(MouseButton::Right, {400, 287});
    app.insertText("1行目");
    app.onKey({KeyCode::Enter});  // 編集中の Enter は改行
    app.insertText("2行目");
    app.onKey({KeyCode::Escape});
    CHECK(app.annotations().specs->back().text == "1行目\n2行目");
    CHECK(nearly(app.annotations().specs->back().fontSize, 24));

    // 色の変更: ダイアログの結果が以降の編集に使われる。キャンセルなら元のまま
    host.colorChoice = 0x00CC66;
    chooseInToolMenu(app, host, {33 /*色*/, 5 /*直線*/});
    app.onMouseDown(MouseButton::Right, {396, 283});
    app.onMouseUp(MouseButton::Right, {400, 287});
    CHECK(host.colorPickerCount == 1);
    CHECK(host.lastColorPickerInitial == 0xFF3B30);
    CHECK(app.annotations().specs->back().colorRGB == 0x00CC66);
    host.colorChoice = std::nullopt;
    chooseInToolMenu(app, host, {33 /*色 (キャンセル)*/, 5 /*直線*/});
    app.onMouseDown(MouseButton::Right, {396, 283});
    app.onMouseUp(MouseButton::Right, {400, 287});
    CHECK(host.colorPickerCount == 2);
    CHECK(app.annotations().specs->back().colorRGB == 0x00CC66);

    // 設定変更だけしてメニューを閉じる → 何も追加されず設定だけ残る
    const size_t annotationCountBefore = app.annotations().specs->size();
    chooseInToolMenu(app, host, {12 /*太さ3px*/});  // ツールは選ばずに閉じる
    CHECK(app.annotations().specs->size() == annotationCountBefore);
    CHECK(!app.selection().visible);
    CHECK(app.currentTool() == EditTool::Line);  // 直前のツールのまま
    chooseInToolMenu(app, host, {2 /*矩形*/});
    app.onMouseDown(MouseButton::Right, {396, 283});
    app.onMouseUp(MouseButton::Right, {400, 287});
    CHECK(nearly(app.annotations().specs->back().strokeWidth, 3));  // 3px に戻っている

    // 塗りつぶし: 末端 index 33-37 が不透明度 {0,64,128,191,255}、38 が塗りつぶしの色。
    // 色を選ぶと塗りなしのままにならないよう不透明で塗り始める
    chooseInToolMenu(app, host, {36 /*不透明度 128*/, 2 /*矩形*/});
    app.onMouseDown(MouseButton::Right, {396, 283});
    app.onMouseUp(MouseButton::Right, {400, 287});
    CHECK(app.annotations().specs->back().fillAlpha == 128);
    CHECK(app.annotations().specs->back().fillRGB == 0xFFFFFF);  // 既定は白
    host.colorChoice = 0x3366FF;
    chooseInToolMenu(app, host, {34 /*塗りなしへ戻す*/, 39 /*塗りつぶしの色*/, 3 /*楕円*/});
    app.onMouseDown(MouseButton::Right, {396, 283});
    app.onMouseUp(MouseButton::Right, {400, 287});
    {
        const AnnotationSpec& spec = app.annotations().specs->back();
        CHECK(spec.kind == AnnotationSpec::Kind::Ellipse);
        CHECK(spec.fillRGB == 0x3366FF);
        CHECK(spec.fillAlpha == 255);  // 塗りなしから色を選んだので不透明になる
    }

    // オブジェクトメニューからも塗りつぶしを変えられる (17-21 不透明度, 22 色)
    CHECK(app.onMouseDown(MouseButton::Left, {398, 285}));  // 塗ってあるので内部クリックで選択できる
    app.onMouseUp(MouseButton::Left);
    CHECK(app.annotations().selected.has_value());
    host.menuChoice = 19;  // 不透明度 128
    app.onMouseDown(MouseButton::Right, {398, 285});
    app.onMouseUp(MouseButton::Right, {398, 285});
    CHECK(app.annotations().specs->back().fillAlpha == 128);
    app.execute(Command::Undo);
    CHECK(app.annotations().specs->back().fillAlpha == 255);
    host.menuChoice = std::nullopt;  // 以降は menuQueue を使う(設定系は再表示されるため)

    // テキストの枠線: 末端 index 39-44 が太さ {0,1,2,3,5,8}、45 が枠線の色。
    // 枠線ぶん余白が広がるので実測境界も縮む (24x12 の overlay - 余白 (2+太さ/2)*2)
    chooseInToolMenu(app, host, {42 /*枠線 2px*/, 9 /*テキスト*/});
    rasterizer.overlayWidth = 24;
    rasterizer.overlayHeight = 12;
    app.onMouseDown(MouseButton::Right, {396, 283});
    app.onMouseUp(MouseButton::Right, {400, 287});
    app.insertText("枠");
    app.onKey({KeyCode::Escape});
    {
        const AnnotationSpec& spec = app.annotations().specs->back();
        CHECK(nearly(spec.borderWidth, 2));
        CHECK(nearly(spec.p2.x - spec.p1.x, 18) && nearly(spec.p2.y - spec.p1.y, 6));
    }
    chooseInToolMenu(app, host, {40 /*枠線なしへ戻す*/});

    // 確定時の実測(ラスタライズ)失敗はメッセージを出す。入力済みの内容は残す
    rasterizer.ok = false;
    chooseInToolMenu(app, host, {9 /*テキスト*/});
    const size_t countBeforeFail = app.annotations().specs->size();
    app.onMouseDown(MouseButton::Right, {396, 283});
    app.onMouseUp(MouseButton::Right, {400, 287});
    app.insertText("失敗するテキスト");
    app.onKey({KeyCode::Escape});
    CHECK(app.statusBar().leftText == "描画に失敗しました");
    CHECK(app.annotations().specs->size() == countBeforeFail + 1);
    CHECK(app.annotations().specs->back().text == "失敗するテキスト");
    rasterizer.ok = true;
    app.onTimer();

    // 一覧が空のときの移動は無視する。戻る先が無いので、貼り付け画像も編集も捨てない
    // (捨てると二度と表示に戻せなくなる)
    const size_t countBeforeMove = app.annotations().specs->size();
    for (const Command command : {Command::NextImage, Command::PrevImage, Command::FirstImage,
                                  Command::LastImage}) {
        app.execute(command);
        CHECK(app.currentImage() && app.currentImage()->width == 8);
        CHECK(app.annotations().specs->size() == countBeforeMove);
        CHECK(host.lastTitle.find("(クリップボード)") == 0);
    }

    // 一覧があるなら移動でフォルダの画像へ戻り、そのとき編集(注釈含む)は破棄される
    std::mutex mutex;
    std::condition_variable cv;
    bool decoded = false;
    cache.setOnDecoded([&](const std::filesystem::path&) {
        std::lock_guard lock(mutex);
        decoded = true;
        cv.notify_all();
    });
    const std::filesystem::path path = "C:/pics/a.png";
    fileSystem.files = {path};
    app.openPath(path);
    {
        std::unique_lock lock(mutex);
        CHECK(cv.wait_for(lock, std::chrono::seconds(5), [&] { return decoded; }));
    }
    app.onDecodeCompleted();
    CHECK(app.currentImage() && app.currentImage()->width == 1);  // FakeDecoder の 1x1

    app.execute(Command::PasteImage);  // 8x8 を貼り直して注釈を 1 つ足す
    app.execute(Command::SelectToolRect);
    app.onMouseDown(MouseButton::Right, {396, 283});
    app.onMouseUp(MouseButton::Right, {400, 287});
    CHECK(app.annotations().specs->size() == 1);
    app.execute(Command::NextImage);  // 1 枚しかないが貼り付け表示からは戻る
    CHECK(app.statusBar().leftText == "編集を破棄しました");
    CHECK(app.currentImage() && app.currentImage()->width == 1);
    CHECK(app.annotations().specs->empty());
    CHECK(host.lastTitle.find("a.png") != std::string::npos);
    app.execute(Command::Undo);
    CHECK(app.statusBar().leftText == "取り消す編集はありません");
}

void testUnicode() {
    // ASCII・日本語・サロゲートペア(絵文字)の往復
    const std::string utf8 = "ABC 日本語テスト 😀";
    CHECK(wideToUtf8(utf8ToWide(utf8)) == utf8);
    CHECK(utf32ToUtf8(utf8ToUtf32(utf8)) == utf8);

    // コードポイント数の検証(😀 は1コードポイント)
    CHECK(utf8ToUtf32("😀").size() == 1);
    CHECK(utf8ToUtf32("日本語").size() == 3);

    // wchar_t のサイズに応じた表現(Windows: UTF-16 サロゲートペア、他: UTF-32)
    const std::wstring wide = utf8ToWide("😀");
    if constexpr (sizeof(wchar_t) == 2) {
        CHECK(wide.size() == 2);
        CHECK(wide[0] == wchar_t(0xD83D) && wide[1] == wchar_t(0xDE00));
    } else {
        CHECK(wide.size() == 1);
        CHECK(static_cast<char32_t>(wide[0]) == U'\U0001F600');
    }

    // 不正な UTF-8 は U+FFFD に置換される(例外なし・停止しない)
    CHECK(utf8ToUtf32("\x80").front() == char32_t(0xFFFD));          // 継続バイト単独
    CHECK(utf8ToUtf32("\xC2").front() == char32_t(0xFFFD));          // 途切れた2バイト列
    CHECK(utf8ToUtf32("\xED\xA0\x80").front() == char32_t(0xFFFD));  // サロゲート領域
    CHECK(utf8ToUtf32("\xC0\xAF").front() == char32_t(0xFFFD));      // 冗長表現 (overlong)

    // パス変換の往復(日本語ファイル名)
    const std::filesystem::path p = pathFromUtf8("フォルダ/画像 (1).png");
    CHECK(pathToUtf8Generic(p) == "フォルダ/画像 (1).png");
}

void testUtf16Offsets() {
    // "あ" は UTF-8 で 3 バイト / UTF-16 で 1 単位、"😀" は 4 バイト / 2 単位
    const std::string s = "aあ😀b";
    CHECK(utf8ToUtf16Offset(s, 0) == 0);
    CHECK(utf8ToUtf16Offset(s, 1) == 1);   // 'a' の後
    CHECK(utf8ToUtf16Offset(s, 4) == 2);   // "あ" の後
    CHECK(utf8ToUtf16Offset(s, 8) == 4);   // "😀" の後(サロゲートペアで +2)
    CHECK(utf8ToUtf16Offset(s, 9) == 5);   // 末尾
    CHECK(utf8ToUtf16Offset(s, 999) == 5); // 範囲外は末尾へ丸める

    CHECK(utf16ToUtf8Offset(s, 0) == 0);
    CHECK(utf16ToUtf8Offset(s, 1) == 1);
    CHECK(utf16ToUtf8Offset(s, 2) == 4);
    CHECK(utf16ToUtf8Offset(s, 4) == 8);
    CHECK(utf16ToUtf8Offset(s, 999) == s.size());
    // サロゲートペアの途中(3)を指したらペアの先頭へ切り下げる
    CHECK(utf16ToUtf8Offset(s, 3) == 4);
}

void testTextEditBuffer() {
    // 構築時のキャレットは末尾。挿入はキャレット位置に入る
    TextEditBuffer buf("あい");
    CHECK(buf.caret() == 6 && !buf.hasSelection());
    buf.insert("う");
    CHECK(buf.text() == "あいう" && buf.caret() == 9);

    // Backspace / Delete はマルチバイト 1 文字ずつ動く
    CHECK(buf.backspace());
    CHECK(buf.text() == "あい" && buf.caret() == 6);
    buf.setCaret(0, false);
    CHECK(!buf.backspace());  // 先頭では何も起きない
    CHECK(buf.deleteForward());
    CHECK(buf.text() == "い" && buf.caret() == 0);
    buf.setCaret(buf.text().size(), false);
    CHECK(!buf.deleteForward());  // 末尾では何も起きない

    // 左右移動もコードポイント単位。範囲外へは出ない
    TextEditBuffer moves("a😀b");
    moves.setCaret(0, false);
    moves.moveRight(false);
    CHECK(moves.caret() == 1);
    moves.moveRight(false);
    CHECK(moves.caret() == 5);  // 絵文字は 4 バイトまとめて飛ぶ
    moves.moveLeft(false);
    CHECK(moves.caret() == 1);
    moves.moveLeft(false);
    moves.moveLeft(false);
    CHECK(moves.caret() == 0);

    // Shift 付き移動で選択が伸び、選択中の挿入は置き換えになる
    TextEditBuffer sel("abcdef");
    sel.setCaret(1, false);
    sel.moveRight(true);
    sel.moveRight(true);
    CHECK(sel.hasSelection() && sel.selectedText() == "bc");
    CHECK(sel.selectionBegin() == 1 && sel.selectionEnd() == 3);
    sel.insert("X");
    CHECK(sel.text() == "aXdef" && !sel.hasSelection() && sel.caret() == 2);

    // 選択中の Backspace は選択を消すだけ(直前の文字は消さない)
    sel.selectAll();
    CHECK(sel.selectedText() == "aXdef");
    CHECK(sel.backspace());
    CHECK(sel.text().empty() && sel.caret() == 0);

    // 選択解除の左右移動は選択の端へ寄る
    TextEditBuffer collapse("abcdef");
    collapse.setCaret(1, false);
    collapse.setCaret(4, true);
    collapse.moveLeft(false);
    CHECK(collapse.caret() == 1 && !collapse.hasSelection());
    collapse.setCaret(1, false);
    collapse.setCaret(4, true);
    collapse.moveRight(false);
    CHECK(collapse.caret() == 4 && !collapse.hasSelection());

    // Home / End は論理行(LF 区切り)の端へ動く
    TextEditBuffer lines("abc\ndef");
    lines.setCaret(5, false);  // 2 行目の 'e' の後
    lines.moveLineStart(false);
    CHECK(lines.caret() == 4);
    lines.moveLineEnd(false);
    CHECK(lines.caret() == 7);
    lines.setCaret(1, false);
    lines.moveLineEnd(false);
    CHECK(lines.caret() == 3);  // LF の手前で止まる
    lines.moveLineStart(false);
    CHECK(lines.caret() == 0);

    // 語の選択: 空白・ASCII 英数字・それ以外の連なりをそれぞれ 1 語として扱う
    TextEditBuffer words("abc def");
    words.selectWordAt(1);
    CHECK(words.selectedText() == "abc");
    words.selectWordAt(3);
    CHECK(words.selectedText() == " ");
    words.selectWordAt(5);
    CHECK(words.selectedText() == "def");
    words.selectWordAt(999);  // 末尾クリックは直前の語
    CHECK(words.selectedText() == "def");
    TextEditBuffer jp("あいう abc");
    jp.selectWordAt(0);
    CHECK(jp.selectedText() == "あいう");  // 非 ASCII の連なりはまとめて 1 語
    TextEditBuffer empty("");
    empty.selectWordAt(0);
    CHECK(!empty.hasSelection());

    // 位置はコードポイント境界へ丸められる(マルチバイトの途中を指しても壊れない)
    TextEditBuffer clamp("あ");
    clamp.setCaret(2, false);
    CHECK(clamp.caret() == 0);
    clamp.setCaret(999, false);
    CHECK(clamp.caret() == 3);
}

void testTextStyleRuns() {
    // 隣り合う同じ書式はまとめ、既定のままの範囲と空の範囲は捨てる
    std::vector<TextStyleRun> runs{{0, 3, true, false, false, false, 0},
                                   {3, 6, true, false, false, false, 0},
                                   {6, 6, true, false, false, false, 0},
                                   {7, 9, false, false, false, false, 0}};
    normalizeTextStyles(runs);
    CHECK(runs.size() == 1);
    CHECK((runs[0] == TextStyleRun{0, 6, true, false, false, false, 0}));

    // 範囲の一部にかける → 前後が切り出され、重ならない 3 範囲になる
    std::vector<TextStyleRun> split{{0, 10, true, false, false, false, 0}};
    setTextStyleColor(split, 3, 6, 0x00FF00);
    CHECK(split.size() == 3);
    CHECK((split[0] == TextStyleRun{0, 3, true, false, false, false, 0}));
    CHECK((split[1] == TextStyleRun{3, 6, true, false, false, true, 0x00FF00}));
    CHECK((split[2] == TextStyleRun{6, 10, true, false, false, false, 0}));

    // 色と太字は独立。書式の無い隙間にも新しい範囲ができる
    std::vector<TextStyleRun> gap{{0, 2, false, false, false, true, 0xFF0000}};
    setTextStyleFlag(gap, 0, 5, TextStyleFlag::Bold, true);
    CHECK(gap.size() == 2);
    CHECK((gap[0] == TextStyleRun{0, 2, true, false, false, true, 0xFF0000}));
    CHECK((gap[1] == TextStyleRun{2, 5, true, false, false, false, 0}));

    // 範囲全体に効いているかの判定(隙間があれば false)
    std::vector<TextStyleRun> partial{{0, 3, true, false, false, false, 0}};
    CHECK(isTextStyleFlagSet(partial, 0, 3, TextStyleFlag::Bold));
    CHECK(!isTextStyleFlagSet(partial, 0, 4, TextStyleFlag::Bold));
    CHECK(!isTextStyleFlagSet(partial, 0, 3, TextStyleFlag::Underline));
    CHECK(!isTextStyleFlagSet(partial, 2, 2, TextStyleFlag::Bold));  // 空範囲は false

    // 解除して既定に戻った範囲は消える(他の属性が残っていれば消えない)
    std::vector<TextStyleRun> off{{0, 3, true, true, false, false, 0}};
    setTextStyleFlag(off, 0, 3, TextStyleFlag::Bold, false);
    CHECK(off.size() == 1);
    CHECK((off[0] == TextStyleRun{0, 3, false, true, false, false, 0}));
    setTextStyleFlag(off, 0, 3, TextStyleFlag::Italic, false);
    CHECK(off.empty());

    // フォントも他の属性と独立。指定を外した範囲は(他が無ければ)消える
    std::vector<TextStyleRun> font{{0, 6, false, false, true, false, 0}};
    setTextStyleFontFamily(font, 2, 4, "Meiryo");
    CHECK(font.size() == 3);
    CHECK((font[1] == TextStyleRun{2, 4, false, false, true, false, 0, "Meiryo"}));
    setTextStyleFontFamily(font, 0, 6, "MS Mincho");
    CHECK(font.size() == 1);  // 全体が同じ書式になったのでまとまる
    CHECK((font[0] == TextStyleRun{0, 6, false, false, true, false, 0, "MS Mincho"}));
    setTextStyleFontFamily(font, 0, 6, "");
    CHECK((font[0] == TextStyleRun{0, 6, false, false, true, false, 0}));
    setTextStyleFlag(font, 0, 6, TextStyleFlag::Underline, false);
    CHECK(font.empty());

    // 指定位置の書式(どの範囲にも無ければ既定)
    const std::vector<TextStyleRun> at{{2, 5, false, false, true, false, 0}};
    CHECK(textStyleAt(at, 3).underline);
    CHECK(!textStyleAt(at, 5).underline);

    // 挿入への追従: 手前は不動、後ろはずれる、挿入位置で終わる範囲は取り込む
    std::vector<TextStyleRun> ins{{0, 3, true, false, false, false, 0},
                                  {5, 8, false, false, true, false, 0}};
    adjustTextStyles(ins, 3, 0, 2);
    CHECK((ins[0] == TextStyleRun{0, 5, true, false, false, false, 0}));
    CHECK((ins[1] == TextStyleRun{7, 10, false, false, true, false, 0}));

    // 挿入位置から始まる範囲は取り込まず、まるごと後ろへずれる
    std::vector<TextStyleRun> after{{3, 6, true, false, false, false, 0}};
    adjustTextStyles(after, 3, 0, 2);
    CHECK((after[0] == TextStyleRun{5, 8, true, false, false, false, 0}));

    // 削除への追従: またがる範囲は縮み、丸ごと消えた範囲は落ちる
    std::vector<TextStyleRun> del{{0, 4, true, false, false, false, 0},
                                  {6, 8, false, false, true, false, 0},
                                  {10, 12, false, false, false, true, 0x0000FF}};
    adjustTextStyles(del, 2, 6, 0);  // [2, 8) を削除
    CHECK(del.size() == 2);
    CHECK((del[0] == TextStyleRun{0, 2, true, false, false, false, 0}));
    CHECK((del[1] == TextStyleRun{4, 6, false, false, false, true, 0x0000FF}));
}

void testTextEditBufferStyles() {
    // 選択範囲のトグル。全体が太字なら解除、一部だけなら全体へ適用する
    TextEditBuffer buf("abcdef");
    buf.setCaret(1, false);
    buf.setCaret(3, true);
    CHECK(buf.toggleSelectionFlag(TextStyleFlag::Bold));
    CHECK(buf.styles().size() == 1);
    CHECK((buf.styles()[0] == TextStyleRun{1, 3, true, false, false, false, 0}));
    CHECK(buf.selectionHasFlag(TextStyleFlag::Bold));
    buf.setCaret(0, false);
    buf.setCaret(4, true);
    CHECK(!buf.selectionHasFlag(TextStyleFlag::Bold));  // 一部だけ太字
    CHECK(buf.toggleSelectionFlag(TextStyleFlag::Bold));
    CHECK((buf.styles()[0] == TextStyleRun{0, 4, true, false, false, false, 0}));
    CHECK(buf.toggleSelectionFlag(TextStyleFlag::Bold));  // もう一度で解除
    CHECK(buf.styles().empty());

    // 選択が無ければ何もしない
    TextEditBuffer none("abc");
    CHECK(!none.toggleSelectionFlag(TextStyleFlag::Bold));
    CHECK(!none.setSelectionColor(0x123456));
    CHECK(!none.setSelectionFontFamily("Meiryo"));
    CHECK(none.styles().empty());

    // 選択範囲だけのフォント変更。同じ指定を繰り返しても変化なしを返す
    TextEditBuffer font("abcdef");
    font.setCaret(1, false);
    font.setCaret(4, true);
    CHECK(font.setSelectionFontFamily("Meiryo"));
    CHECK(font.styles().size() == 1);
    CHECK((font.styles()[0] == TextStyleRun{1, 4, false, false, false, false, 0, "Meiryo"}));
    CHECK(font.selectionStyle().fontFamily == "Meiryo");
    CHECK(!font.setSelectionFontFamily("Meiryo"));
    // 文字列の編集にも他の書式と同じように追従する
    font.setCaret(0, false);
    font.insert("XY");
    CHECK((font.styles()[0] == TextStyleRun{3, 6, false, false, false, false, 0, "Meiryo"}));

    // 色・太字・斜体・下線は同じ範囲に共存でき、独立にトグルできる
    TextEditBuffer both("abcdef");
    both.setCaret(2, false);
    both.setCaret(5, true);
    CHECK(both.setSelectionColor(0xFF8800));
    CHECK(both.toggleSelectionFlag(TextStyleFlag::Underline));
    CHECK(both.toggleSelectionFlag(TextStyleFlag::Italic));
    CHECK(both.toggleSelectionFlag(TextStyleFlag::Bold));
    CHECK(both.styles().size() == 1);
    CHECK((both.styles()[0] == TextStyleRun{2, 5, true, true, true, true, 0xFF8800}));
    CHECK(both.selectionStyle().colorRGB == 0xFF8800);
    // 斜体だけ外しても他は残る
    CHECK(both.toggleSelectionFlag(TextStyleFlag::Italic));
    CHECK((both.styles()[0] == TextStyleRun{2, 5, true, false, true, true, 0xFF8800}));
    CHECK(!both.selectionHasFlag(TextStyleFlag::Italic));
    CHECK(both.selectionHasFlag(TextStyleFlag::Bold));

    // 文字列を編集しても書式は同じ文字に付いたまま追従する
    TextEditBuffer edit("abcdef");
    edit.setCaret(3, false);
    edit.setCaret(6, true);
    edit.toggleSelectionFlag(TextStyleFlag::Bold);
    edit.setCaret(0, false);
    edit.insert("XY");  // 先頭に 2 バイト挿入 → 太字は [5, 8) へ
    CHECK(edit.text() == "XYabcdef");
    CHECK((edit.styles()[0] == TextStyleRun{5, 8, true, false, false, false, 0}));
    edit.setCaret(0, false);
    edit.deleteForward();  // 先頭 1 バイト削除 → [4, 7) へ
    CHECK((edit.styles()[0] == TextStyleRun{4, 7, true, false, false, false, 0}));

    // 太字部分の直後で入力した文字は太字を継ぐ(直前の書式を引き継ぐ)
    TextEditBuffer inherit("abc");
    inherit.selectAll();
    inherit.toggleSelectionFlag(TextStyleFlag::Bold);
    inherit.setCaret(3, false);
    inherit.insert("d");
    CHECK((inherit.styles()[0] == TextStyleRun{0, 4, true, false, false, false, 0}));
    // 太字部分の直前で入力した文字は継がない
    inherit.setCaret(0, false);
    inherit.insert("Z");
    CHECK((inherit.styles()[0] == TextStyleRun{1, 5, true, false, false, false, 0}));

    // 選択範囲を置き換えると、その範囲に付いていた書式は消える
    TextEditBuffer replace("abcdef");
    replace.setCaret(2, false);
    replace.setCaret(4, true);
    replace.toggleSelectionFlag(TextStyleFlag::Underline);
    replace.setCaret(2, false);
    replace.setCaret(4, true);
    replace.insert("ZZ");
    CHECK(replace.text() == "abZZef");
    CHECK(replace.styles().empty());

    // 構築時に書式を受け取る(注釈からの再編集)
    TextEditBuffer restored("abcdef", {{0, 3, true, false, false, false, 0}});
    CHECK(restored.styles().size() == 1);
    CHECK(restored.selectionStyle().begin == 6);  // キャレットは末尾で選択なし
}

void testAppTextEditing() {
    FakeDecoder decoder;
    ImageCache cache(decoder);
    FakeHost host;
    FakeFileSystem fs;
    FakeClipboard clipboard;
    FakeEncoder encoder;
    FakeAnnotationRasterizer rasterizer;
    FakeOcrEngine ocrEngine;
    OcrService ocrService(ocrEngine);
    ScanService scanService(fs);
    FakePrinter printer;
    App app(host, fs, cache, clipboard, encoder, rasterizer, ocrService, printer,
            scanService);

    app.onResize(800, 600);
    // 100x100 の画像を貼り付け、等倍表示にしてスクリーン座標を素直にする
    auto source = std::make_shared<DecodedImage>();
    source->width = 100;
    source->height = 100;
    source->pixels.resize(100 * 100 * 4);
    clipboard.pasteImage = source;
    app.execute(Command::PasteImage);
    app.execute(Command::ZoomActual);
    const Matrix3x2 toScreen = app.imageToScreen();
    // 実測境界 40x20(テキスト余白 2px x 両側)。枠を現実的な高さにしてクリック判定を安定させる
    rasterizer.overlayWidth = 44;
    rasterizer.overlayHeight = 24;
    const auto screenOf = [&toScreen](float x, float y) { return toScreen.apply({x, y}); };

    // テキストボックスを作って入力する
    chooseInToolMenu(app, host, {9});  // テキストツールへ切り替える
    app.onMouseDown(MouseButton::Right, screenOf(10, 10));
    app.onMouseUp(MouseButton::Right, screenOf(50, 30));
    CHECK(app.isTextEditing());
    app.insertText("abcdef");
    CHECK(app.annotations().specs->back().text == "abcdef");

    // キャレットは末尾。編集ビューにはキャレットの位置が入る(選択中は非表示)
    {
        const AnnotationsView view = app.annotations();
        CHECK(view.textEdit.active);
        CHECK(view.textEdit.index == 0);
        CHECK(view.textEdit.selectionRects.empty());
        // 枠の左端 (10) + 6 文字 x 10px
        CHECK(nearly(view.textEdit.caretTop.x, 70));
    }

    // 左矢印でキャレットが戻り、Shift + 左で選択が伸びる
    app.onKey({KeyCode::Left});
    app.onKey({KeyChord{KeyCode::Left, false, true, false}});
    {
        const AnnotationsView view = app.annotations();
        CHECK(!view.textEdit.caretVisible);  // 選択中はキャレットを出さない
        CHECK(view.textEdit.selectionRects.size() == 1);
        CHECK(nearly(view.textEdit.selectionRects[0].left, 10 + 40));
        CHECK(nearly(view.textEdit.selectionRects[0].right, 10 + 50));
    }

    // 選択範囲の切り取り → クリップボードへ渡り、本文からは消える
    app.onKey({KeyChord{static_cast<KeyCode>('X'), true, false, false}});
    CHECK(clipboard.lastText == "e");
    CHECK(app.annotations().specs->back().text == "abcdf");

    // 貼り付けはキャレット位置に入る
    clipboard.pasteText = "XY";
    app.onKey({KeyChord{static_cast<KeyCode>('V'), true, false, false}});
    CHECK(app.annotations().specs->back().text == "abcdXYf");

    // 全選択して置き換え
    app.onKey({KeyChord{static_cast<KeyCode>('A'), true, false, false}});
    app.insertText("Z");
    CHECK(app.annotations().specs->back().text == "Z");

    // 枠内クリックでキャレットが動き、ドラッグで範囲選択できる
    app.onKey({KeyChord{static_cast<KeyCode>('A'), true, false, false}});
    app.insertText("0123456789");
    app.onMouseDown(MouseButton::Left, screenOf(10 + 25, 15));  // 枠内 2.5 文字目 → 境界 3
    CHECK(app.isTextEditing());
    app.onMouseMove(screenOf(10 + 55, 15));  // 5.5 文字目 → 境界 6
    app.onMouseUp(MouseButton::Left);
    app.onKey({KeyChord{static_cast<KeyCode>('C'), true, false, false}});
    CHECK(clipboard.lastText == "345");  // [3,6) の 3 文字

    // 編集中の枠内では I ビームカーソルを出す(枠外・画像の別の場所では出さない)
    CHECK(app.wantsTextCursor(screenOf(30, 20)));   // 枠 (10,10)-(50,30) の内側
    CHECK(!app.wantsTextCursor(screenOf(90, 90)));  // 枠の外

    // IME の変換中文字列はキャレット位置にインライン表示され、確定するまで
    // 編集内容には入らない
    app.onKey({KeyCode::Home});  // 選択を解除してキャレットを先頭へ
    app.beginComposition();
    app.setComposition("にほんご", 12, 0, 6);  // キャレットは末尾、前半2文字が変換対象
    CHECK(app.isComposing());
    {
        const AnnotationsView view = app.annotations();
        // 表示は「変換中文字列 + 確定済みテキスト」。注釈の text も表示用になる
        CHECK(app.annotations().specs->back().text == "にほんご0123456789");
        CHECK(view.textEdit.compositionRects.size() == 1);
        CHECK(nearly(view.textEdit.compositionRects[0].left, 10 + 0));
        CHECK(nearly(view.textEdit.compositionRects[0].right, 10 + 40));  // 4 文字 x 10px
        // 変換対象の節(前半 2 文字)だけ太い下線になる
        CHECK(view.textEdit.compositionTargetRects.size() == 1);
        CHECK(nearly(view.textEdit.compositionTargetRects[0].right, 10 + 20));
        // キャレットは変換中文字列の末尾(4 文字目の後ろ)
        CHECK(nearly(view.textEdit.caretTop.x, 10 + 40));
    }

    // 変換を取り消すと表示も元に戻る(編集内容は変わっていない)
    app.clearComposition();
    CHECK(!app.isComposing());
    CHECK(app.annotations().specs->back().text == "0123456789");
    CHECK(app.annotations().textEdit.compositionRects.empty());

    // 確定すると変換中文字列が編集内容へ入る
    app.beginComposition();
    app.setComposition("にほん", 9, 0, 9);
    app.insertText("日本");  // IME の確定文字列
    CHECK(!app.isComposing());
    CHECK(app.annotations().specs->back().text == "日本0123456789");

    // 変換中のキー入力は IME が扱うので App は編集しない
    app.setComposition("あ", 3, 0, 0);  // キャレットは "日本" の直後
    app.onKey({KeyCode::Backspace});
    app.onKey({KeyCode::Delete});
    CHECK(app.annotations().specs->back().text == "日本あ0123456789");
    app.clearComposition();
    CHECK(app.annotations().specs->back().text == "日本0123456789");

    // 選択範囲があるときに変換を始めると選択は置き換えられる
    app.onKey({KeyChord{static_cast<KeyCode>('A'), true, false, false}});
    app.beginComposition();
    CHECK(app.annotations().specs->back().text.empty());
    app.insertText("再入力");
    CHECK(app.annotations().specs->back().text == "再入力");
    app.onKey({KeyChord{static_cast<KeyCode>('A'), true, false, false}});
    app.insertText("0123456789");  // 後続のテストのため元の内容へ戻す

    // 編集中はコマンドが暴発しない(次の画像へ移動しない・タイトルが変わらない)
    const std::string titleWhileEditing = host.lastTitle;
    app.onKey({KeyCode::Space});
    CHECK(app.isTextEditing());
    CHECK(host.lastTitle == titleWhileEditing);

    // キャレットは点滅する(タイマー通知で表示相が入れ替わる)
    app.onKey({KeyCode::End});  // 選択を解除してキャレットを出す
    const bool before = app.annotations().textEdit.caretVisible;
    app.onCaretBlink();
    CHECK(app.annotations().textEdit.caretVisible != before);

    // 枠の外のクリックで確定して編集が終わる
    app.onMouseDown(MouseButton::Left, screenOf(90, 90));
    CHECK(!app.isTextEditing());
    CHECK(!host.textEditing);
    CHECK(app.annotations().specs->back().text == "0123456789");
    CHECK(!app.annotations().textEdit.active);
    // 確定後は枠の内側でも I ビームにしない(編集していないテキストは通常のカーソル)
    CHECK(!app.wantsTextCursor(screenOf(30, 20)));

    // 編集中の Ctrl+Z は編集開始前へ戻す(新規作成なら注釈ごと消える)
    const size_t countBeforeUndo = app.annotations().specs->size();
    app.onMouseDown(MouseButton::Right, screenOf(10, 60));  // ツールはテキストのまま(切り替え不要)
    app.onMouseUp(MouseButton::Right, screenOf(50, 80));
    app.insertText("捨てる");
    app.onKey({KeyChord{static_cast<KeyCode>('Z'), true, false, false}});
    CHECK(!app.isTextEditing());
    CHECK(app.annotations().specs->size() == countBeforeUndo);

    // 画像が切り替わると編集は破棄され、host にも終了が伝わる
    app.onMouseDown(MouseButton::Right, screenOf(10, 60));
    app.onMouseUp(MouseButton::Right, screenOf(50, 80));
    app.insertText("破棄される");
    CHECK(app.isTextEditing());
    fs.files = {"a.png"};
    app.openPath("a.png");
    app.onDecodeCompleted();
    CHECK(!app.isTextEditing());
    CHECK(!host.textEditing);
    CHECK(app.annotations().specs->empty());
}

void testAppTextStyles() {
    FakeDecoder decoder;
    ImageCache cache(decoder);
    FakeHost host;
    FakeFileSystem fs;
    FakeClipboard clipboard;
    FakeEncoder encoder;
    FakeAnnotationRasterizer rasterizer;
    FakeOcrEngine ocrEngine;
    OcrService ocrService(ocrEngine);
    ScanService scanService(fs);
    FakePrinter printer;
    App app(host, fs, cache, clipboard, encoder, rasterizer, ocrService, printer,
            scanService);

    app.onResize(800, 600);
    auto source = std::make_shared<DecodedImage>();
    source->width = 100;
    source->height = 100;
    source->pixels.resize(100 * 100 * 4);
    clipboard.pasteImage = source;
    app.execute(Command::PasteImage);
    app.execute(Command::ZoomActual);
    const Matrix3x2 toScreen = app.imageToScreen();
    rasterizer.overlayWidth = 44;
    rasterizer.overlayHeight = 24;
    const auto screenOf = [&toScreen](float x, float y) { return toScreen.apply({x, y}); };
    const auto ctrl = [](char c) {
        return KeyChord{static_cast<KeyCode>(c), true, false, false};
    };

    chooseInToolMenu(app, host, {9});  // テキストツールへ切り替える
    app.onMouseDown(MouseButton::Right, screenOf(10, 10));
    app.onMouseUp(MouseButton::Right, screenOf(50, 30));
    app.insertText("abcdef");

    // Ctrl+B は選択部分だけを太字にする(選択が無ければ何も起きない)
    app.onKey(ctrl('B'));
    CHECK(app.annotations().specs->back().styles.empty());
    app.onKey(ctrl('A'));
    app.onKey(ctrl('B'));
    CHECK(app.annotations().specs->back().styles.size() == 1);
    CHECK((app.annotations().specs->back().styles[0] ==
          TextStyleRun{0, 6, true, false, false, false, 0}));
    app.onKey(ctrl('B'));  // もう一度で解除
    CHECK(app.annotations().specs->back().styles.empty());

    // Ctrl+I / Ctrl+U は斜体・下線。前半 3 文字だけを選び直してかける
    app.onKey({KeyCode::Home});
    for (int i = 0; i < 3; ++i) app.onKey({KeyChord{KeyCode::Right, false, true, false}});
    app.onKey(ctrl('I'));
    CHECK((app.annotations().specs->back().styles[0] ==
          TextStyleRun{0, 3, false, true, false, false, 0}));
    app.onKey(ctrl('I'));  // もう一度で解除
    CHECK(app.annotations().specs->back().styles.empty());
    app.onKey(ctrl('U'));
    CHECK(app.annotations().specs->back().styles.size() == 1);
    CHECK((app.annotations().specs->back().styles[0] ==
          TextStyleRun{0, 3, false, false, true, false, 0}));

    // 選択範囲の上での右クリックは編集を確定せず、書式メニューを出す
    const int menusBefore = host.menuCount;
    host.menuChoice = std::nullopt;  // キャンセル
    app.onMouseDown(MouseButton::Right, screenOf(20, 15));
    app.onMouseUp(MouseButton::Right, screenOf(20, 15));
    CHECK(app.isTextEditing());  // 右クリックで編集が終わらない
    CHECK(host.menuCount == menusBefore + 1);
    // 末端 index: 0-2 太字・斜体・下線, 3-11 フォント(候補9種), 12 文字色
    CHECK(countMenuLeaves(host.lastMenuItems) == 13);
    CHECK(!host.lastMenuItems[0].checked);            // 太字は付いていない
    CHECK(!host.lastMenuItems[1].checked);            // 斜体も付いていない
    CHECK(host.lastMenuItems[2].checked);             // 下線は付いている

    // メニューから斜体を選ぶと、選択部分だけ斜体になる(下線はそのまま)
    host.menuChoice = 1;  // 斜体
    app.onMouseDown(MouseButton::Right, screenOf(20, 15));
    app.onMouseUp(MouseButton::Right, screenOf(20, 15));
    CHECK((app.annotations().specs->back().styles[0] ==
          TextStyleRun{0, 3, false, true, true, false, 0}));
    app.onKey(ctrl('I'));  // 斜体を戻して以降のテストを素直にする

    // メニューから文字色を選ぶと、選択部分だけ色が付く(下線はそのまま)
    host.menuChoice = 12;  // 文字色...
    host.colorChoice = 0x00FF00;
    app.onMouseDown(MouseButton::Right, screenOf(20, 15));
    app.onMouseUp(MouseButton::Right, screenOf(20, 15));
    CHECK(app.annotations().specs->back().styles.size() == 1);
    CHECK((app.annotations().specs->back().styles[0] ==
          TextStyleRun{0, 3, false, false, true, true, 0x00FF00}));
    // 色を指定していないので、ピッカーの初期値は注釈全体の色
    CHECK(host.lastColorPickerInitial == app.annotations().specs->back().colorRGB);

    // メニューからフォントを選ぶと選択部分だけ書体が変わる(他の書式・全体のフォントは不変)
    host.menuChoice = 6;  // フォント: 3 游ゴシック, 4 游ゴシック UI, 5 游明朝, 6 メイリオ
    app.onMouseDown(MouseButton::Right, screenOf(20, 15));
    app.onMouseUp(MouseButton::Right, screenOf(20, 15));
    CHECK(app.annotations().specs->back().styles.size() == 1);
    CHECK((app.annotations().specs->back().styles[0] ==
          TextStyleRun{0, 3, false, false, true, true, 0x00FF00, "Meiryo"}));
    CHECK(app.annotations().specs->back().fontFamily == kDefaultFontFamily);

    // 見出しとチェックは選択範囲に付いているフォントに追従する
    host.menuChoice = std::nullopt;
    app.onMouseDown(MouseButton::Right, screenOf(20, 15));
    app.onMouseUp(MouseButton::Right, screenOf(20, 15));
    {
        const MenuItem* family = findMenuItem(host.lastMenuItems, "フォント (");
        CHECK(family != nullptr);
        if (family) {
            CHECK(family->text == "フォント (メイリオ)");
            CHECK(family->children[3].checked);   // メイリオ
            CHECK(!family->children[0].checked);  // 游ゴシック
        }
    }

    // 注釈全体と同じフォントを選ぶと指定が外れる(他の書式は残る)
    host.menuChoice = 3;  // 游ゴシック = 注釈全体のフォント
    app.onMouseDown(MouseButton::Right, screenOf(20, 15));
    app.onMouseUp(MouseButton::Right, screenOf(20, 15));
    CHECK((app.annotations().specs->back().styles[0] ==
          TextStyleRun{0, 3, false, false, true, true, 0x00FF00}));

    // 確定しても書式は残り、再編集でも引き継がれる
    app.onKey({KeyCode::Escape});
    CHECK(!app.isTextEditing());
    CHECK(app.annotations().specs->back().styles.size() == 1);
    app.onDoubleClick(screenOf(20, 15));
    CHECK(app.isTextEditing());
    app.onKey(ctrl('A'));
    CHECK(app.annotations().specs->back().styles[0].underline);

    // 枠の外での右クリックは従来どおり編集を確定する
    host.menuChoice = std::nullopt;
    app.onMouseDown(MouseButton::Right, screenOf(90, 90));
    app.onMouseUp(MouseButton::Right, screenOf(90, 90));
    CHECK(!app.isTextEditing());

    // テキスト注釈を「選択中」の Ctrl+B は、サイドバー開閉ではなく全体の太字トグル
    CHECK(!app.sidebar().visible);
    CHECK(app.onMouseDown(MouseButton::Left, screenOf(20, 15)));  // 編集はせず選択だけ
    app.onMouseUp(MouseButton::Left);
    CHECK(!app.isTextEditing());
    CHECK(app.annotations().selected.has_value());
    const size_t textBytes = app.annotations().specs->back().text.size();
    CHECK(app.onKey(ctrl('B')));
    CHECK(!app.sidebar().visible);  // サイドバーは開かない
    {
        // 全体が太字になり、元からあった部分書式(前半3文字の下線・色)は保たれる
        const std::vector<TextStyleRun>& styles = app.annotations().specs->back().styles;
        CHECK(isTextStyleFlagSet(styles, 0, textBytes, TextStyleFlag::Bold));
        CHECK(textStyleAt(styles, 0).underline);
        CHECK(textStyleAt(styles, 0).colorRGB == 0x00FF00);
        CHECK(!textStyleAt(styles, textBytes - 1).underline);  // 後半は太字だけ
    }
    app.onKey(ctrl('B'));  // もう一度で解除(全体が太字なら外す)
    CHECK(!app.sidebar().visible);
    CHECK(!isTextStyleFlagSet(app.annotations().specs->back().styles, 0, textBytes,
                              TextStyleFlag::Bold));
    app.execute(Command::Undo);  // 太字の付け外しは undo できる
    CHECK(isTextStyleFlagSet(app.annotations().specs->back().styles, 0, textBytes,
                             TextStyleFlag::Bold));

    // 図形を選んでいるとき・何も選んでいないときは従来どおりサイドバーが開く
    app.execute(Command::Escape);
    CHECK(!app.annotations().selected.has_value());
    app.onKey(ctrl('B'));
    CHECK(app.sidebar().visible);
    app.onKey(ctrl('B'));
    CHECK(!app.sidebar().visible);
}

void testAppFontFamily() {
    FakeDecoder decoder;
    ImageCache cache(decoder);
    FakeHost host;
    FakeFileSystem fs;
    FakeClipboard clipboard;
    FakeEncoder encoder;
    FakeAnnotationRasterizer rasterizer;
    FakeOcrEngine ocrEngine;
    OcrService ocrService(ocrEngine);
    ScanService scanService(fs);
    FakePrinter printer;
    App app(host, fs, cache, clipboard, encoder, rasterizer, ocrService, printer,
            scanService);

    app.onResize(800, 600);
    auto source = std::make_shared<DecodedImage>();
    source->width = 100;
    source->height = 100;
    source->pixels.resize(100 * 100 * 4);
    clipboard.pasteImage = source;
    app.execute(Command::PasteImage);
    app.execute(Command::ZoomActual);
    const Matrix3x2 toScreen = app.imageToScreen();
    rasterizer.overlayWidth = 44;
    rasterizer.overlayHeight = 24;
    const auto screenOf = [&toScreen](float x, float y) { return toScreen.apply({x, y}); };
    // テキストを 1 つ作って確定する
    const auto addText = [&](const char* body) {
        app.onMouseDown(MouseButton::Right, screenOf(10, 10));
        app.onMouseUp(MouseButton::Right, screenOf(50, 30));
        app.insertText(body);
        app.onKey({KeyCode::Escape});
    };

    // 設定の適用ではシステムフォントを列挙しない(問い合わせはメニューを開いたときだけ)
    app.applyConfig(Config::parse("[edit]\ncolor = 112233\n"));
    CHECK(rasterizer.hasFontFamilyCount == 0);

    // 既定のフォントは新規テキストへそのまま載る
    chooseInToolMenu(app, host, {9 /*テキスト*/});
    CHECK(rasterizer.hasFontFamilyCount > 0);
    addText("あ");
    CHECK(app.annotations().specs->back().fontFamily == kDefaultFontFamily);

    // ツールメニューのフォント(末端 index 23-31)から選ぶと以降の新規テキストへ効く
    chooseInToolMenu(app, host, {27 /*メイリオ*/});
    addText("い");
    CHECK(app.annotations().specs->back().fontFamily == "Meiryo");
    CHECK(app.annotations().specs->front().fontFamily == kDefaultFontFamily);  // 既存は不変

    // オブジェクトメニュー(テキスト)の末端 index:
    // 0 編集, 1 削除, 2-9 回転, 10-16 文字サイズ, 17-25 フォント, 26 色,
    // 27-31 塗りつぶし, 32 塗りつぶしの色, 33-38 枠線, 39 枠線の色
    CHECK(app.onMouseDown(MouseButton::Left, screenOf(20, 15)));  // 直近のテキストを選択
    app.onMouseUp(MouseButton::Left);
    host.menuChoice = 18;  // 游ゴシック UI
    app.onMouseDown(MouseButton::Right, screenOf(20, 15));
    app.onMouseUp(MouseButton::Right, screenOf(20, 15));
    CHECK(countMenuLeaves(host.lastMenuItems) == 40);
    CHECK(app.annotations().specs->back().fontFamily == "Yu Gothic UI");
    app.execute(Command::Undo);
    CHECK(app.annotations().specs->back().fontFamily == "Meiryo");

    // ini では候補表にない名前も指定できる(入っていなければ描画側がフォールバックする)
    app.applyConfig(Config::parse("[edit]\nfont_family = BIZ UDPMincho\n"));
    addText("う");
    CHECK(app.annotations().specs->back().fontFamily == "BIZ UDPMincho");

    // 入っていないフォントは候補から外れ、候補表にない現在のフォントは末尾に足される
    // (選び直したあとで戻れるように)
    rasterizer.missingFonts = {"Yu Mincho", "MS Mincho"};
    host.menuChoice = std::nullopt;
    app.onMouseDown(MouseButton::Right, kEmptySpot);
    app.onMouseUp(MouseButton::Right, kEmptySpot);
    const MenuItem* family = findMenuItem(host.lastMenuItems, "フォント (");
    CHECK(family != nullptr);
    if (family) {
        CHECK(family->text == "フォント (BIZ UDPMincho)");
        CHECK(family->children.size() == 8);  // 候補 9 - 欠け 2 + 現在のフォント 1
        CHECK(family->children.back().text == "BIZ UDPMincho");
        CHECK(family->children.back().checked);
        CHECK(findMenuItem(family->children, "游明朝") == nullptr);
        CHECK(findMenuItem(family->children, "メイリオ") != nullptr);
    }
}

void testAppPasteObject() {
    FakeDecoder decoder;
    ImageCache cache(decoder);
    FakeHost host;
    FakeFileSystem fileSystem;
    FakeClipboard clipboard;
    FakeEncoder encoder;
    FakeAnnotationRasterizer rasterizer;
    FakeOcrEngine ocrEngine;
    OcrService ocrService(ocrEngine);
    ScanService scanService(fileSystem);
    FakePrinter printer;
    App app(host, fileSystem, cache, clipboard, encoder, rasterizer, ocrService, printer,
            scanService);
    app.onResize(800, 600);

    const auto makeImage = [](uint32_t w, uint32_t h) {
        auto image = std::make_shared<DecodedImage>();
        image->width = w;
        image->height = h;
        image->pixels.resize(static_cast<size_t>(w) * h * 4);
        return image;
    };

    // 下地が無いうちは Ctrl+V と同じ(オブジェクトにする相手がいない)
    const std::shared_ptr<DecodedImage> base = makeImage(100, 100);
    clipboard.pasteImage = base;
    app.execute(Command::PasteObject);
    CHECK(app.currentImage() == base);
    CHECK(app.annotations().specs->empty());

    app.execute(Command::ZoomActual);  // 等倍・中央表示にして座標を素直にする
    const Matrix3x2 toScreen = app.imageToScreen();
    const auto screenOf = [&toScreen](float x, float y) { return toScreen.apply({x, y}); };

    // 40x20 をオブジェクトとして貼る。下地の画素は変わらない
    const std::shared_ptr<DecodedImage> pasted = makeImage(40, 20);
    clipboard.pasteImage = pasted;
    app.execute(Command::PasteObject);
    CHECK(app.currentImage() == base);
    CHECK(app.statusBar().leftText == "40 x 20 px の画像を貼り付けました");
    {
        const AnnotationsView view = app.annotations();
        CHECK(view.specs->size() == 1);
        CHECK(view.selected && *view.selected == 0);  // 貼った直後から掴める
        const AnnotationSpec& spec = view.specs->back();
        CHECK(spec.kind == AnnotationSpec::Kind::Image);
        CHECK(spec.image == pasted);  // 画素は複製せず共有する
        // 可視領域の中心(= 画像の中心 (50,50))へ等倍で置かれる
        CHECK(nearly(spec.p1.x, 30) && nearly(spec.p1.y, 40));
        CHECK(nearly(spec.p2.x, 70) && nearly(spec.p2.y, 60));
    }

    // オブジェクトメニューは削除と回転だけ(線・色・塗りつぶしは画像に効かない)
    host.menuChoice = std::nullopt;
    app.onMouseDown(MouseButton::Right, screenOf(50, 50));
    app.onMouseUp(MouseButton::Right, screenOf(50, 50));
    CHECK(countMenuLeaves(host.lastMenuItems) == 9);  // 削除 1 + 回転 8
    CHECK(findMenuItem(host.lastMenuItems, "線の太さ") == nullptr);
    CHECK(findMenuItem(host.lastMenuItems, "色の変更") == nullptr);
    CHECK(findMenuItem(host.lastMenuItems, "塗りつぶし") == nullptr);
    CHECK(findMenuItem(host.lastMenuItems, "回転角度") != nullptr);

    // 選択中はオブジェクトだけがコピーされる(下地は含まない)
    rasterizer.overlayWidth = 40;
    rasterizer.overlayHeight = 20;
    app.execute(Command::CopyImage);
    CHECK(rasterizer.rasterizeCount == 1);
    CHECK(rasterizer.lastSpec.kind == AnnotationSpec::Kind::Image);
    CHECK(clipboard.lastWidth == 40 && clipboard.lastHeight == 20);
    CHECK(app.statusBar().leftText == "オブジェクトをクリップボードにコピーしました");

    // 選択を外せば下地に焼き込んだ 1 枚(100x100)へ戻る
    app.execute(Command::Escape);
    app.execute(Command::CopyImage);
    CHECK(rasterizer.rasterizeCount == 2);
    CHECK(clipboard.lastWidth == 100 && clipboard.lastHeight == 100);
    CHECK(app.statusBar().leftText == "画像をクリップボードにコピーしました");
    rasterizer.overlayWidth = 1;
    rasterizer.overlayHeight = 1;

    // ラスタライズに失敗したら下地で代用せず、失敗として知らせる
    app.onMouseDown(MouseButton::Left, screenOf(50, 50));  // オブジェクトを選び直す
    app.onMouseUp(MouseButton::Left, screenOf(50, 50));
    CHECK(app.annotations().selected == std::optional<size_t>(0));
    rasterizer.ok = false;
    app.execute(Command::CopyImage);
    CHECK(app.statusBar().leftText == "オブジェクトのコピーに失敗しました");
    rasterizer.ok = true;

    // 他の注釈と同じく undo/redo できる
    app.execute(Command::Undo);
    CHECK(app.annotations().specs->empty());
    CHECK(app.currentImage() == base);
    app.execute(Command::Redo);
    CHECK(app.annotations().specs->size() == 1);

    // 焼き込みの上限を超える画像は、取り込む時点で縮められる(あとでは画素が戻らない)
    clipboard.pasteImage = makeImage(kMaxResizeDimension + 1000, 1);
    app.execute(Command::PasteObject);
    CHECK(app.annotations().specs->size() == 2);
    CHECK(app.annotations().specs->back().image->width == kMaxResizeDimension);

    // クリップボードが空なら何も起きない
    clipboard.pasteImage = nullptr;
    app.execute(Command::PasteObject);
    CHECK(app.annotations().specs->size() == 2);
    CHECK(app.statusBar().leftText == "クリップボードに画像がありません");

    // 注釈を扱えない環境では、見えないオブジェクトを作らず画像として開く
    rasterizer.supported = false;
    clipboard.pasteImage = pasted;
    app.execute(Command::PasteObject);
    CHECK(app.currentImage() == pasted);
    CHECK(app.annotations().specs->empty());
    CHECK(app.statusBar().leftText ==
          "この環境では画像オブジェクトを扱えないため、画像として開きました");
}

void testAppPenTools() {
    FakeDecoder decoder;
    ImageCache cache(decoder);
    FakeHost host;
    FakeFileSystem fileSystem;
    FakeClipboard clipboard;
    FakeEncoder encoder;
    FakeAnnotationRasterizer rasterizer;
    FakeOcrEngine ocrEngine;
    OcrService ocrService(ocrEngine);
    ScanService scanService(fileSystem);
    FakePrinter printer;
    App app(host, fileSystem, cache, clipboard, encoder, rasterizer, ocrService, printer,
            scanService);
    app.onResize(800, 600);

    auto source = std::make_shared<DecodedImage>();
    source->width = 40;
    source->height = 40;
    source->pixels.resize(40 * 40 * 4);
    clipboard.pasteImage = source;
    app.execute(Command::PasteImage);
    // 40x40 は等倍で中央に置かれる(画像 (0,0) はスクリーン (380,267))
    const auto screenOf = [](float x, float y) { return Point{380 + x, 267 + y}; };

    // ペン: 右ドラッグの軌跡がそのまま注釈になる。近すぎる通過点は間引かれる
    app.execute(Command::SelectToolPen);
    CHECK(app.currentTool() == EditTool::Pen);
    app.onMouseDown(MouseButton::Right, screenOf(0, 0));
    app.onMouseMove(screenOf(1, 0));  // 直前から 1px = 間引かれる
    app.onMouseMove(screenOf(20, 0));
    app.onMouseMove(screenOf(20, 20));
    CHECK(app.annotations().preview != nullptr);  // ドラッグ中も実物が見える
    CHECK(app.annotations().preview->points.size() == 3);
    CHECK(!app.selection().visible);  // ラバーバンドとは排他
    app.onMouseUp(MouseButton::Right, screenOf(20, 20));
    CHECK(app.annotations().specs->size() == 1);
    {
        const AnnotationSpec& spec = app.annotations().specs->back();
        CHECK(spec.kind == AnnotationSpec::Kind::Pen);
        CHECK(spec.points.size() == 3);  // 終点は既に入っているので重複しない
        CHECK(nearly(spec.points.back().x, 20) && nearly(spec.points.back().y, 20));
        CHECK(nearly(spec.p1.x, 0) && nearly(spec.p1.y, 0));
        CHECK(nearly(spec.p2.x, 20) && nearly(spec.p2.y, 20));
        CHECK(nearly(spec.strokeWidth, 3));  // 既定の太さ(画像px)
        CHECK(spec.strokeAlpha == 255);
    }

    // 線の上だけ掴める(bbox の内側でも線から離れていればパンに回る)。
    // 選択中はハンドルが優先されるので、いったん選択を外してから掴む
    app.execute(Command::Escape);
    CHECK(app.onMouseDown(MouseButton::Left, screenOf(10, 0)));
    CHECK(app.annotations().selected == std::optional<size_t>(0));
    app.onMouseMove(screenOf(12, 3));  // +2,+3 の移動は点列ごと動く
    {
        const AnnotationSpec& spec = app.annotations().specs->back();
        CHECK(nearly(spec.points.front().x, 2) && nearly(spec.points.front().y, 3));
        CHECK(nearly(spec.p1.x, 2) && nearly(spec.p1.y, 3));
    }
    app.onMouseUp(MouseButton::Left);
    app.execute(Command::Undo);
    CHECK(nearly(app.annotations().specs->back().points.front().x, 0));
    CHECK(!app.onMouseDown(MouseButton::Left, screenOf(10, 10)));  // L の内側は掴めない
    app.onMouseUp(MouseButton::Left);

    // やり直し: 取り消した移動を復元する。新しい編集をすると redo は捨てられる
    app.execute(Command::Redo);
    CHECK(nearly(app.annotations().specs->back().points.front().x, 2));
    app.execute(Command::Undo);
    CHECK(nearly(app.annotations().specs->back().points.front().x, 0));

    // マーカー: 同じ Pen 注釈だが太く半透明になる
    app.execute(Command::SelectToolMarker);
    app.onMouseDown(MouseButton::Right, screenOf(0, 30));
    app.onMouseMove(screenOf(30, 30));
    app.onMouseUp(MouseButton::Right, screenOf(30, 30));
    CHECK(app.annotations().specs->size() == 2);
    {
        const AnnotationSpec& spec = app.annotations().specs->back();
        CHECK(spec.kind == AnnotationSpec::Kind::Pen);
        CHECK(nearly(spec.strokeWidth, 12));  // 既定 3px の 4 倍
        CHECK(spec.strokeAlpha == 102);
    }
    // 新しい編集をしたので、さっきの取り消しはもうやり直せない
    app.execute(Command::Redo);
    CHECK(app.statusBar().leftText == "やり直す編集はありません");
    app.onTimer();

    // 連番マーカー: 番号は自動で増え、小さすぎるドラッグでも最小の大きさになる
    app.execute(Command::SelectToolNumber);
    app.onMouseDown(MouseButton::Right, screenOf(0, 0));
    app.onMouseUp(MouseButton::Right, screenOf(5, 5));
    {
        const AnnotationSpec& spec = app.annotations().specs->back();
        CHECK(spec.kind == AnnotationSpec::Kind::Number);
        CHECK(spec.number == 1);
        CHECK(spec.fillAlpha == 255);
        CHECK(spec.fillRGB == 0xFF3B30);  // 既定の色が円の塗りになる
        CHECK(nearly(spec.p2.x - spec.p1.x, spec.p2.y - spec.p1.y));  // 必ず円
        CHECK(spec.p2.x - spec.p1.x > 30);  // 文字サイズ 18px から決まる最小の直径
    }
    app.onMouseDown(MouseButton::Right, screenOf(0, 20));
    app.onMouseUp(MouseButton::Right, screenOf(6, 26));
    CHECK(app.annotations().specs->back().number == 2);
    // 取り消して置き直しても番号は詰まる(状態ではなく既存の注釈から数えている)
    app.execute(Command::Undo);
    app.onMouseDown(MouseButton::Right, screenOf(0, 20));
    app.onMouseUp(MouseButton::Right, screenOf(6, 26));
    CHECK(app.annotations().specs->back().number == 2);
    // Shift なしでもドラッグが正方形に寄せられる(円をつぶせない)
    app.onMouseDown(MouseButton::Right, screenOf(0, 0));
    app.onMouseMove(screenOf(39, 35));  // 縦横の小さいほう = 35 の正方形になる
    CHECK(app.annotations().preview != nullptr);
    CHECK(nearly(app.annotations().preview->p2.x, 35));
    CHECK(nearly(app.annotations().preview->p2.y, 35));
    app.onMouseUp(MouseButton::Right, screenOf(39, 35));

    // オブジェクトメニューから番号を振り直せる(末端 index: 0 削除, 1-8 回転,
    // 9-15 太さ, 16-25 番号, 26 色, 27-31 塗りつぶし, 32 塗りつぶしの色)
    CHECK(app.onMouseDown(MouseButton::Left, screenOf(5, 5)));
    app.onMouseUp(MouseButton::Left);
    host.menuChoice = 22;  // 番号 7
    app.onMouseDown(MouseButton::Right, screenOf(5, 5));
    app.onMouseUp(MouseButton::Right, screenOf(5, 5));
    CHECK(countMenuLeaves(host.lastMenuItems) == 33);
    CHECK(app.annotations().specs->back().number == 7);

    // Shift ドラッグ: 手書きも矢印と同じく水平 / 垂直 / 45 度へ寄せてまっすぐ引ける
    app.execute(Command::SelectToolPen);
    app.onMouseDown(MouseButton::Right, screenOf(0, 0));
    app.onMouseMove(screenOf(10, 2), true);  // 11 度 → 水平
    CHECK(app.annotations().preview->points.size() == 2);  // 通過点ではなく直線 1 本
    CHECK(nearly(app.annotations().preview->points.back().x, 10));
    CHECK(nearly(app.annotations().preview->points.back().y, 0));
    app.onMouseUp(MouseButton::Right, screenOf(30, 5), true);
    {
        const AnnotationSpec& spec = app.annotations().specs->back();
        CHECK(spec.points.size() == 2);
        CHECK(nearly(spec.points.back().x, 30) && nearly(spec.points.back().y, 0));
    }
    app.execute(Command::Undo);

    // 途中から Shift を押すと、そこまでの軌跡は残したまま以降だけがまっすぐになる。
    // 離せばまた自由に描ける(アンカーは押し直すたびに取り直す)
    app.onMouseDown(MouseButton::Right, screenOf(0, 0));
    app.onMouseMove(screenOf(0, 10));
    app.onMouseMove(screenOf(30, 12), true);  // (0,10) から水平へ
    CHECK(app.annotations().preview->points.size() == 3);
    CHECK(nearly(app.annotations().preview->points.back().x, 30));
    CHECK(nearly(app.annotations().preview->points.back().y, 10));
    app.onMouseMove(screenOf(32, 20));  // Shift を離した後は素通し
    app.onMouseUp(MouseButton::Right, screenOf(32, 20));
    {
        const AnnotationSpec& spec = app.annotations().specs->back();
        CHECK(spec.points.size() == 4);
        CHECK(nearly(spec.points[2].x, 30) && nearly(spec.points[2].y, 10));
        CHECK(nearly(spec.points.back().x, 32) && nearly(spec.points.back().y, 20));
    }
    app.execute(Command::Undo);

    // ini からもツールを選べる
    app.applyConfig(Config::parse("[edit]\ntool = marker\n"));
    CHECK(app.currentTool() == EditTool::Marker);
    app.applyConfig(Config::parse("[edit]\ntool = number\n"));
    CHECK(app.currentTool() == EditTool::Number);
    app.applyConfig(Config::parse("[edit]\ntool = pen\n"));
    CHECK(app.currentTool() == EditTool::Pen);
}

// 線幅・文字サイズは画像px基準。描いたときの表示倍率に影響されない
void testAnnotationSizeIsImageBased() {
    FakeDecoder decoder;
    ImageCache cache(decoder);
    FakeHost host;
    FakeFileSystem fileSystem;
    FakeClipboard clipboard;
    FakeEncoder encoder;
    FakeAnnotationRasterizer rasterizer;
    FakeOcrEngine ocrEngine;
    OcrService ocrService(ocrEngine);
    ScanService scanService(fileSystem);
    FakePrinter printer;
    App app(host, fileSystem, cache, clipboard, encoder, rasterizer, ocrService, printer,
            scanService);
    app.onResize(800, 600);

    // 画面中央付近で描いても縁に当たらないよう、余裕のある大きさにする
    auto source = std::make_shared<DecodedImage>();
    source->width = 400;
    source->height = 400;
    source->pixels.resize(400 * 400 * 4);
    clipboard.pasteImage = source;
    app.execute(Command::PasteImage);
    CHECK(nearly(app.zoom(), 1.0f));  // 400x400 は 800x600 に収まるので等倍

    // 等倍で 1 本引く。既定の 3px がそのまま画像座標に入る
    app.execute(Command::SelectToolRect);
    app.onMouseDown(MouseButton::Right, {390, 277});
    app.onMouseUp(MouseButton::Right, {400, 287});
    CHECK(nearly(app.annotations().specs->back().strokeWidth, 3));

    // 拡大してから引いても同じ太さ(以前は 1/zoom されて細くなっていた)
    app.execute(Command::ZoomIn);
    app.execute(Command::ZoomIn);
    CHECK(app.zoom() > 1.0f);
    app.onMouseDown(MouseButton::Right, {390, 277});
    app.onMouseUp(MouseButton::Right, {400, 287});
    CHECK(nearly(app.annotations().specs->back().strokeWidth, 3));

    // 縮小側も同じ
    app.execute(Command::ZoomOut);
    app.execute(Command::ZoomOut);
    app.execute(Command::ZoomOut);
    app.execute(Command::ZoomOut);
    CHECK(app.zoom() < 1.0f);
    app.onMouseDown(MouseButton::Right, {395, 282});
    app.onMouseUp(MouseButton::Right, {400, 287});
    CHECK(nearly(app.annotations().specs->back().strokeWidth, 3));

    // マーカーの 4 倍も、連番マーカーの最小直径(文字サイズ由来)も倍率に依らない
    app.execute(Command::SelectToolMarker);
    app.onMouseDown(MouseButton::Right, {395, 282});
    app.onMouseMove({400, 287});
    app.onMouseUp(MouseButton::Right, {400, 287});
    CHECK(nearly(app.annotations().specs->back().strokeWidth, 12));

    app.execute(Command::SelectToolNumber);
    app.onMouseDown(MouseButton::Right, {380, 270});
    app.onMouseUp(MouseButton::Right, {386, 276});
    {
        const AnnotationSpec& spec = app.annotations().specs->back();
        CHECK(spec.kind == AnnotationSpec::Kind::Number);
        CHECK(nearly(spec.p2.x - spec.p1.x, 18 * 1.8f));  // 文字サイズ 18px の 1.8 倍
    }
}

// [mouse] swap_buttons = true はパンと編集だけを入れ替える(メニューは常に右クリック)
void testAppSwapMouseButtons() {
    FakeDecoder decoder;
    ImageCache cache(decoder);
    FakeHost host;
    FakeFileSystem fileSystem;
    FakeClipboard clipboard;
    FakeEncoder encoder;
    FakeAnnotationRasterizer rasterizer;
    FakeOcrEngine ocrEngine;
    OcrService ocrService(ocrEngine);
    ScanService scanService(fileSystem);
    FakePrinter printer;
    App app(host, fileSystem, cache, clipboard, encoder, rasterizer, ocrService, printer,
            scanService);
    app.onResize(800, 600);

    // 既定では左がパン・右が編集
    CHECK(app.mouseRole(MouseButton::Left) == MouseRole::Pan);
    CHECK(app.mouseRole(MouseButton::Right) == MouseRole::Edit);
    app.applyConfig(Config::parse("[mouse]\nswap_buttons = true\n"));
    CHECK(app.mouseRole(MouseButton::Left) == MouseRole::Edit);
    CHECK(app.mouseRole(MouseButton::Right) == MouseRole::Pan);

    // 右ドラッグがパンになる(ウィンドウより大きい画像を等倍表示して動かせる状態にする)
    auto wide = std::make_shared<DecodedImage>();
    wide->width = 4000;
    wide->height = 400;
    wide->pixels.resize(static_cast<size_t>(4000) * 400 * 4);
    clipboard.pasteImage = wide;
    app.execute(Command::PasteImage);
    app.execute(Command::ZoomActual);
    const float originX = app.imageToScreen().apply({0, 0}).x;
    CHECK(!app.onMouseDown(MouseButton::Right, kEmptySpot));  // 何も掴まない = パンになる
    app.onMouseMove({kEmptySpot.x + 30, kEmptySpot.y});
    app.onMouseUp(MouseButton::Right, {kEmptySpot.x + 30, kEmptySpot.y});
    CHECK(nearly(app.imageToScreen().apply({0, 0}).x, originX + 30));
    CHECK(app.annotations().specs->empty());  // パンなので注釈は作らない
    CHECK(host.menuCount == 0);               // ドラッグしたのでメニューも出ない

    // 8x8 画像を貼り付ける(等倍のまま中央 = 画像左上はスクリーン (396,283))
    auto source = std::make_shared<DecodedImage>();
    source->width = 8;
    source->height = 8;
    source->pixels.resize(8 * 8 * 4);
    clipboard.pasteImage = source;
    app.execute(Command::PasteImage);
    CHECK(nearly(app.imageToScreen().apply({0, 0}).x, 396));

    // 左ドラッグが現在のツール(矩形)を実行する
    CHECK(app.onMouseDown(MouseButton::Left, {396, 283}));
    app.onMouseMove({400, 287});
    app.onMouseUp(MouseButton::Left, {400, 287});
    CHECK(app.annotations().specs->size() == 1);
    CHECK(host.menuCount == 0);

    // 左クリック(ドラッグなし)は何も作らず、メニューも出さない
    app.onMouseDown(MouseButton::Left, kEmptySpot);
    app.onMouseUp(MouseButton::Left, kEmptySpot);
    CHECK(app.annotations().specs->size() == 1);
    CHECK(host.menuCount == 0);

    // 右クリック(ドラッグなし)は入れ替えてもツール切り替えメニュー
    host.menuChoice = std::nullopt;
    app.onMouseDown(MouseButton::Right, kEmptySpot);
    app.onMouseUp(MouseButton::Right, kEmptySpot);
    CHECK(host.menuCount == 1);
    CHECK(findMenuItem(host.lastMenuItems, "矩形") != nullptr);
    CHECK(app.annotations().specs->size() == 1);

    // 図形を掴む操作は入れ替えの対象外。入れ替えても左クリックで選択して移動できる
    // (閾値を超えて動かしたので、左が編集役でも新しい矩形は作られない)
    constexpr Point corner{396, 283};  // 矩形の角 = 画像 (0,0)
    CHECK(app.onMouseDown(MouseButton::Left, corner));
    CHECK(app.annotations().selected == std::optional<size_t>(0));
    app.onMouseMove({corner.x + 6, corner.y + 6});
    app.onMouseUp(MouseButton::Left, {corner.x + 6, corner.y + 6});
    CHECK(app.annotations().specs->size() == 1);
    CHECK(nearly(app.annotations().specs->front().p1.x, 6));
    CHECK(host.menuCount == 1);

    // 図形の上でもパン役(入れ替え時は右)のドラッグはパンのまま。掴まないので図形は動かない
    {
        constexpr Point moved{corner.x + 6, corner.y + 6};
        CHECK(!app.onMouseDown(MouseButton::Right, moved));  // false = パンを始めた
        app.onMouseMove({moved.x + 20, moved.y});
        app.onMouseUp(MouseButton::Right, {moved.x + 20, moved.y});
        CHECK(nearly(app.annotations().specs->front().p1.x, 6));  // 図形は動いていない
        CHECK(app.annotations().specs->size() == 1);
    }

    // 図形の上での右クリック(ドラッグなし)はオブジェクトメニュー
    host.menuChoice = 0;  // 削除
    constexpr Point movedCorner{corner.x + 6, corner.y + 6};
    app.onMouseDown(MouseButton::Right, movedCorner);
    app.onMouseUp(MouseButton::Right, movedCorner);
    CHECK(host.menuCount == 2);
    CHECK(app.annotations().specs->empty());

    // サイドバーは UI 部品なので入れ替えの対象外(左クリックで項目へ移動、
    // 右クリックは swap_buttons によらず一覧のメニュー)
    app.applyConfig(Config::parse("[view]\nsidebar = true\n"));
    host.menuChoice = std::nullopt;  // キャンセルするので並び順は変わらない
    CHECK(app.onMouseDown(MouseButton::Left, {100, 100}));
    app.onMouseUp(MouseButton::Left, {100, 100});
    CHECK(app.onMouseDown(MouseButton::Right, {100, 100}));
    app.onMouseUp(MouseButton::Right, {100, 100});
    CHECK(host.menuCount == 3);  // 一覧のメニュー(並び替え・サブフォルダ)
    CHECK(findMenuItem(host.lastMenuItems, "昇順") != nullptr);
}

void testNaturalCompare() {
    // 数字の連続は数値として比較(エクスプローラ相当)
    CHECK(naturalCompare("1.png", "2.png") < 0);
    CHECK(naturalCompare("2.png", "10.png") < 0);
    CHECK(naturalCompare("a2b", "a10b") < 0);
    CHECK(naturalCompare("img9.png", "img10.png") < 0);
    // 大文字小文字は無視(ASCII)
    CHECK(naturalCompare("ABC", "abc") == 0);
    CHECK(naturalCompare("a2", "B1") < 0);
    // 数値が同じなら先頭ゼロが少ない方が先
    CHECK(naturalCompare("1", "01") < 0);
    CHECK(naturalCompare("01.png", "01.png") == 0);
    // 前置詞・長さの違い
    CHECK(naturalCompare("abc", "abcd") < 0);
    CHECK(naturalCompare("", "a") < 0);
    // 非 ASCII (UTF-8) はバイト順 = コードポイント順
    CHECK(naturalCompare("あ", "い") < 0);
}

void testOcrText() {
    // 両隣が CJK の空白だけを落とす。和欧の境目や欧文どうしの語間は残す
    CHECK(removeSpacesBetweenCjk("これ は 文字 認識 です") == "これは文字認識です");
    CHECK(removeSpacesBetweenCjk("ABC あいう") == "ABC あいう");
    CHECK(removeSpacesBetweenCjk("あいう ABC") == "あいう ABC");
    CHECK(removeSpacesBetweenCjk("hello world") == "hello world");
    CHECK(removeSpacesBetweenCjk("漢字   漢字") == "漢字漢字");  // 連続する空白もまとめて
    CHECK(removeSpacesBetweenCjk("あ\tい") == "あい");           // タブも同じ扱い
    CHECK(removeSpacesBetweenCjk(" あ ") == " あ ");             // 片側しか無い空白は残す
    CHECK(removeSpacesBetweenCjk("") == "");
    CHECK(removeSpacesBetweenCjk("ハングル 한글 です") == "ハングル한글です");

    // 改行はまたがない(行の連結は ocrResultToText が先に済ませる)
    CHECK(removeSpacesBetweenCjk("あ\n い") == "あ\n い");

    OcrResult result;
    result.lines.push_back({"  こんにちは 世界  ", {}});
    result.lines.push_back({"   ", {}});  // 空白だけの行は落ちる
    result.lines.push_back({"second line", {}});
    CHECK(ocrResultToText(result) == "こんにちは世界\nsecond line");

    CHECK(ocrResultToText(OcrResult{}).empty());
}

void testOcrRetryUpscale() {
    constexpr double kNoLimit = 1e9;
    const auto same = [](double a, double b) { return std::abs(a - b) < 1e-9; };

    // 行が無い / 既に十分大きいときは読み直さない
    CHECK(same(ocrRetryUpscale(0, kNoLimit), 1.0));
    CHECK(same(ocrRetryUpscale(-5, kNoLimit), 1.0));
    CHECK(same(ocrRetryUpscale(20, kNoLimit), 1.0));
    CHECK(same(ocrRetryUpscale(100, kNoLimit), 1.0));

    // 小さい行は目標 30px になる倍率で読み直す
    CHECK(same(ocrRetryUpscale(10, kNoLimit), 3.0));
    CHECK(same(ocrRetryUpscale(15, kNoLimit), 2.0));

    // 伸びしろが小さい(1.3 倍未満)なら 1 回分の時間に見合わないので読み直さない
    CHECK(ocrRetryUpscale(19, kNoLimit) > 1.0);   // 30/19 = 1.58 なので読み直す
    CHECK(same(ocrRetryUpscale(10, 1.2), 1.0));   // 上限に阻まれて 1.3 倍に届かない

    // 拡大しすぎは精度が落ちるので 4 倍で頭打ち
    CHECK(same(ocrRetryUpscale(1, kNoLimit), 4.0));
    CHECK(same(ocrRetryUpscale(2, kNoLimit), 4.0));

    // 認識器のサイズ上限は目標より優先される
    CHECK(same(ocrRetryUpscale(10, 2.0), 2.0));
    CHECK(same(ocrRetryUpscale(10, 5.0), 3.0));  // 上限が緩ければ目標どおり
}

void testFlattenOnBackground() {
    // 半透明を含まない画像はそのまま(検出だけで判断できる)
    DecodedImage opaque;
    opaque.width = 1;
    opaque.height = 1;
    opaque.pixels = {10, 20, 30, 255};
    CHECK(!hasTransparency(opaque));

    DecodedImage src;
    src.width = 3;
    src.height = 1;
    // 完全透明・半透明(事前乗算で 50%)・不透明
    src.pixels = {0, 0, 0, 0, 64, 64, 64, 128, 1, 2, 3, 255};
    CHECK(hasTransparency(src));

    const auto white = flattenOnBackground(src, 0xFFFFFF);
    CHECK(white != nullptr);
    CHECK(white->width == 3 && white->height == 1);
    // 完全透明は背景色そのもの
    CHECK(white->pixels[0] == 255 && white->pixels[1] == 255 && white->pixels[2] == 255);
    CHECK(white->pixels[3] == 255);
    // 半透明は 64 + 255 * (127/255) = 191
    CHECK(white->pixels[4] == 191 && white->pixels[7] == 255);
    // 不透明は元のまま
    CHECK(white->pixels[8] == 1 && white->pixels[9] == 2 && white->pixels[10] == 3);
    CHECK(white->pixels[11] == 255);
    // 元の画像は壊さない
    CHECK(src.pixels[3] == 0);

    // 黒背景なら透明部分は黒(= 焼き込まないときと同じ)になる
    const auto black = flattenOnBackground(src, 0x000000);
    CHECK(black != nullptr);
    CHECK(black->pixels[0] == 0 && black->pixels[3] == 255);

    CHECK(flattenOnBackground(DecodedImage{}, 0xFFFFFF) == nullptr);
}

void testOcrService() {
    FakeOcrEngine engine;
    engine.lines.push_back({"認識 した 行", {1, 2, 3, 4}});
    OcrService service(engine);

    std::mutex mutex;
    std::condition_variable cv;
    int completions = 0;
    service.setOnCompleted([&] {
        std::lock_guard lock(mutex);
        ++completions;
        cv.notify_all();
    });
    const auto waitForCompletion = [&](int count) {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, std::chrono::seconds(5), [&] { return completions >= count; });
    };

    // 結果を取り出す前は空
    CHECK(!service.takeResult().has_value());

    auto image = std::make_shared<DecodedImage>();
    image->width = 2;
    image->height = 1;
    image->pixels.assign(8, 255);
    const uint64_t generation = service.request(image);
    CHECK(generation != 0);
    CHECK(waitForCompletion(1));

    const auto done = service.takeResult();
    CHECK(done.has_value());
    CHECK(done->generation == generation);
    CHECK(done->ok);
    CHECK(done->result.lines.size() == 1);
    CHECK(done->result.lines[0].bounds.w == 3);
    CHECK(done->result.language == "ja");
    CHECK(engine.lastWidth == 2 && engine.lastHeight == 1);
    // 取り出した結果は消える
    CHECK(!service.takeResult().has_value());

    // 予約ごとに generation が進む
    const uint64_t second = service.request(image);
    CHECK(second > generation);
    CHECK(waitForCompletion(2));
    CHECK(service.takeResult()->generation == second);

    // 失敗は ok = false と理由で返る
    engine.ok = false;
    engine.failureReason = "言語パックがありません";
    service.request(image);
    CHECK(waitForCompletion(3));
    const auto failed = service.takeResult();
    CHECK(failed.has_value());
    CHECK(!failed->ok);
    CHECK(failed->error == "言語パックがありません");

    // 画像が無い予約はエンジンを呼ばずに失敗する
    const int before = engine.recognizeCount;
    service.request(nullptr);
    CHECK(waitForCompletion(4));
    CHECK(engine.recognizeCount == before);
    CHECK(!service.takeResult()->ok);
}

void testAppOcr() {
    FakeDecoder decoder;
    ImageCache cache(decoder);
    FakeHost host;
    FakeFileSystem fileSystem;
    FakeClipboard clipboard;
    FakeEncoder encoder;
    FakeAnnotationRasterizer rasterizer;
    FakeOcrEngine ocrEngine;
    OcrService ocrService(ocrEngine);
    ScanService scanService(fileSystem);

    std::mutex mutex;
    std::condition_variable cv;
    int completions = 0;
    ocrService.setOnCompleted([&] {
        std::lock_guard lock(mutex);
        ++completions;
        cv.notify_all();
    });
    const auto waitForCompletion = [&](int count) {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, std::chrono::seconds(5), [&] { return completions >= count; });
    };

    FakePrinter printer;
    App app(host, fileSystem, cache, clipboard, encoder, rasterizer, ocrService, printer,
            scanService);
    app.onResize(800, 600);

    // 画像が無いときは予約せず通知だけ
    app.execute(Command::CopyOcrText);
    CHECK(app.statusBar().leftText == "文字を認識する画像がありません");
    CHECK(ocrEngine.recognizeCount == 0);

    // 4x4 の不透明な画像を貼り付けて表示中にする
    auto pasted = std::make_shared<DecodedImage>();
    pasted->width = 4;
    pasted->height = 4;
    pasted->pixels.assign(static_cast<size_t>(4) * 4 * 4, 255);
    clipboard.pasteImage = pasted;
    app.execute(Command::PasteImage);
    CHECK(app.currentImage() != nullptr);

    ocrEngine.lines.push_back({"これ は テスト", {0, 0, 4, 2}});
    ocrEngine.lines.push_back({"second", {0, 2, 4, 2}});
    app.execute(Command::CopyOcrText);
    CHECK(app.statusBar().leftText == "文字を認識しています...");
    CHECK(waitForCompletion(1));
    app.onOcrCompleted();
    CHECK(ocrEngine.recognizeCount == 1);
    CHECK(ocrEngine.lastWidth == 4 && ocrEngine.lastHeight == 4);
    // CJK の語間が詰まり、行は改行で連結されてクリップボードへ入る
    CHECK(clipboard.lastText == "これはテスト\nsecond");
    CHECK(app.statusBar().leftText == "2 行をコピーしました (ja)");

    // 1 行も認識できなければクリップボードは触らない
    clipboard.lastText.clear();
    ocrEngine.lines.clear();
    app.execute(Command::CopyOcrText);
    CHECK(waitForCompletion(2));
    app.onOcrCompleted();
    CHECK(clipboard.lastText.empty());
    CHECK(app.statusBar().leftText == "文字を認識できませんでした");

    // 失敗理由はそのままステータスバーへ出る
    ocrEngine.ok = false;
    ocrEngine.failureReason = "言語パックが入っていません";
    app.execute(Command::CopyOcrText);
    CHECK(waitForCompletion(3));
    app.onOcrCompleted();
    CHECK(app.statusBar().leftText == "言語パックが入っていません");

    // 予約し直した後に届いた古い結果は捨てる(前の画像の文字が紛れ込まない)
    ocrEngine.ok = true;
    ocrEngine.lines.push_back({"新しい結果", {}});
    clipboard.lastText.clear();
    app.execute(Command::CopyOcrText);
    CHECK(waitForCompletion(4));
    app.execute(Command::CopyOcrText);  // 世代を進めてから古い結果を流し込む
    CHECK(waitForCompletion(5));
    app.onOcrCompleted();  // 最新の 1 件だけが残っているので採用される
    CHECK(clipboard.lastText == "新しい結果");
    // 2 回目の onOcrCompleted は取り出す結果が無く、状態を変えない
    const std::string after = app.statusBar().leftText;
    app.onOcrCompleted();
    CHECK(app.statusBar().leftText == after);

    // 文字認識ツールの編集ドラッグは、選んだ範囲だけを切り出して渡す。
    // 8x8 の画像を貼り直し、ビューポート中央 (400, 287) を基準に (0,0)-(4,4) を選ぶ
    auto wide = std::make_shared<DecodedImage>();
    wide->width = 8;
    wide->height = 8;
    wide->pixels.assign(static_cast<size_t>(8) * 8 * 4, 255);
    clipboard.pasteImage = wide;
    app.execute(Command::PasteImage);
    app.execute(Command::SelectToolOcr);
    CHECK(app.currentTool() == EditTool::Ocr);

    app.onMouseDown(MouseButton::Right, {396, 283});
    app.onMouseMove({400, 287});
    // トリミングと同じくラバーバンドを出す(図形のプレビューは出ない)
    CHECK(app.selection().visible);
    CHECK(app.annotations().preview == nullptr);
    app.onMouseUp(MouseButton::Right, {400, 287});
    CHECK(waitForCompletion(6));
    app.onOcrCompleted();
    // 切り出した 4x4 だけが渡り、画像そのものは変わらない(トリミングと違う点)
    CHECK(ocrEngine.lastWidth == 4 && ocrEngine.lastHeight == 4);
    CHECK(app.currentImage()->width == 8 && app.currentImage()->height == 8);
    // 続けて別の範囲を読めるようツールは維持される(トリミングは矩形へ戻る)
    CHECK(app.currentTool() == EditTool::Ocr);

    // 画像の外だけを選んだドラッグは予約せず通知だけ
    const int beforeOutside = ocrEngine.recognizeCount;
    app.onMouseDown(MouseButton::Right, {600, 450});
    app.onMouseMove({640, 490});
    app.onMouseUp(MouseButton::Right, {640, 490});
    CHECK(app.statusBar().leftText == "選択した範囲が画像の外です");
    CHECK(ocrEngine.recognizeCount == beforeOutside);

    app.execute(Command::SelectToolRect);

    // 透明部分を持つ画像は白へ焼き込んでから渡す(黒い文字が沈まないように)
    auto transparent = std::make_shared<DecodedImage>();
    transparent->width = 1;
    transparent->height = 1;
    transparent->pixels = {0, 0, 0, 0};
    clipboard.pasteImage = transparent;
    app.execute(Command::PasteImage);
    app.execute(Command::CopyOcrText);
    CHECK(waitForCompletion(7));
    app.onOcrCompleted();
    CHECK(ocrEngine.lastFirstPixel == (std::array<uint8_t, 4>{255, 255, 255, 255}));
}

// 指定色で塗りつぶした画像を作る(合成結果の判別用。PBGRA・事前乗算)
std::shared_ptr<DecodedImage> solidImage(uint32_t width, uint32_t height, uint8_t blue,
                                         uint8_t alpha) {
    auto image = std::make_shared<DecodedImage>();
    image->width = width;
    image->height = height;
    image->pixels.resize(static_cast<size_t>(width) * height * 4);
    for (size_t i = 0; i < image->pixels.size(); i += 4) {
        image->pixels[i + 0] = blue;
        image->pixels[i + 1] = 0;
        image->pixels[i + 2] = 0;
        image->pixels[i + 3] = alpha;
    }
    return image;
}

void testAnimationCore() {
    // 遅延の正規化: 小さすぎる指定は既定値へ読み替える(そのまま従うと数百 fps になる)
    CHECK(normalizedDelayMs(0, 20, 100) == 100);
    CHECK(normalizedDelayMs(10, 20, 100) == 100);
    CHECK(normalizedDelayMs(20, 20, 100) == 20);   // 閾値ちょうどは読み替えない
    CHECK(normalizedDelayMs(500, 20, 100) == 500);
    CHECK(normalizedDelayMs(0, 0, 100) == 1);      // 閾値 0 でも 0ms は返さない

    // 無限ループ (loopCount = 0): 末尾の次は先頭へ戻り、再生は続く
    PlaybackState infinite;
    infinite.playing = true;
    CHECK(advanceFrame(infinite, 3, 0) && infinite.index == 1);
    CHECK(advanceFrame(infinite, 3, 0) && infinite.index == 2);
    CHECK(advanceFrame(infinite, 3, 0) && infinite.index == 0 && infinite.loopsDone == 1);
    CHECK(infinite.playing);

    // 回数指定: 2 周し終えたら最後のフレームを出したまま止まる
    PlaybackState limited;
    limited.playing = true;
    CHECK(advanceFrame(limited, 2, 2) && limited.index == 1);
    CHECK(advanceFrame(limited, 2, 2) && limited.index == 0 && limited.loopsDone == 1);
    CHECK(advanceFrame(limited, 2, 2) && limited.index == 1);
    CHECK(!advanceFrame(limited, 2, 2));  // 2 周目の末尾
    CHECK(!limited.playing && limited.index == 1 && limited.loopsDone == 2);

    // 1 フレームしかないものは再生しない
    PlaybackState single;
    single.playing = true;
    CHECK(!advanceFrame(single, 1, 0) && !single.playing);

    // --- 合成 ---
    const auto blue = solidImage(2, 2, 255, 255);          // 不透明
    const auto clear = solidImage(2, 2, 0, 0);             // 完全透明
    // 指定座標の青成分とアルファを調べる(CHECK はマクロなので、比較まで関数の中で行う)
    const auto pixelIs = [](const DecodedImage& img, uint32_t x, uint32_t y, int blue,
                            int alpha) {
        const size_t i = (static_cast<size_t>(y) * img.width + x) * 4;
        return img.pixels[i + 0] == blue && img.pixels[i + 3] == alpha;
    };

    // None: 描いたものが次のフレームにも残る
    AnimationCompositor keep(4, 4);
    const auto k0 = keep.addFrame(*blue, 0, 0, FrameBlend::Over, FrameDisposal::None);
    CHECK(k0 && k0->width == 4 && k0->height == 4);
    CHECK(pixelIs(*k0, 0, 0, 255, 255));
    CHECK(pixelIs(*k0, 3, 3, 0, 0));  // 触っていない所は透明
    const auto k1 = keep.addFrame(*clear, 2, 2, FrameBlend::Over, FrameDisposal::None);
    CHECK(pixelIs(*k1, 0, 0, 255, 255));  // 透明を重ねても残る

    // Background: そのフレームの矩形だけが透明へ戻る
    AnimationCompositor background(4, 4);
    background.addFrame(*blue, 0, 0, FrameBlend::Over, FrameDisposal::Background);
    const auto b1 = background.addFrame(*clear, 2, 2, FrameBlend::Over, FrameDisposal::None);
    CHECK(pixelIs(*b1, 0, 0, 0, 0));  // 消えている

    // Previous: 描く前のキャンバスへ戻る(1 枚目の絵が復活する)
    AnimationCompositor previous(4, 4);
    previous.addFrame(*blue, 0, 0, FrameBlend::Over, FrameDisposal::None);
    previous.addFrame(*blue, 2, 2, FrameBlend::Over, FrameDisposal::Previous);
    const auto p2 = previous.addFrame(*clear, 0, 0, FrameBlend::Over, FrameDisposal::None);
    CHECK(pixelIs(*p2, 0, 0, 255, 255));  // 1 枚目は残る
    CHECK(pixelIs(*p2, 2, 2, 0, 0));      // 2 枚目は戻された

    // Source: 透明もそのまま置き換える(Over との違い)
    AnimationCompositor source(4, 4);
    source.addFrame(*blue, 0, 0, FrameBlend::Over, FrameDisposal::None);
    const auto s1 = source.addFrame(*clear, 0, 0, FrameBlend::Source, FrameDisposal::None);
    CHECK(pixelIs(*s1, 0, 0, 0, 0));

    // キャンバスからはみ出す配置でも落ちない(切り捨てる)
    AnimationCompositor clipped(4, 4);
    CHECK(clipped.addFrame(*blue, 3, 3, FrameBlend::Over, FrameDisposal::Background) != nullptr);
    CHECK(clipped.addFrame(*blue, -1, -1, FrameBlend::Over, FrameDisposal::None) != nullptr);
    CHECK(clipped.addFrame(*blue, 90, 90, FrameBlend::Over, FrameDisposal::None) != nullptr);

    // 拡張子での絞り込み(これが true のときだけ probeSequence が呼ばれる)
    CHECK(mayHaveMultipleFrames("a.gif") && mayHaveMultipleFrames("a.TIF"));
    CHECK(mayHaveMultipleFrames("a.tiff") && mayHaveMultipleFrames("a.ico"));
    // APNG / アニメ WebP は非対応なので調べない(WIC がフレームを列挙しないため)
    CHECK(!mayHaveMultipleFrames("a.png") && !mayHaveMultipleFrames("a.webp"));
    CHECK(!mayHaveMultipleFrames("a.jpg") && !mayHaveMultipleFrames("a"));

    // [animation] の読み取り
    const Config config = Config::parse(
        "[animation]\nautoplay = false\nloop = false\nmin_delay_ms = 30\n"
        "default_delay_ms = 120\nmax_memory_mb = 64\nmax_frames = 5\n");
    const AnimationOptions options = animationOptionsFromConfig(config);
    CHECK(!options.autoplay && !options.loopForever);
    CHECK(options.minDelayMs == 30 && options.defaultDelayMs == 120);
    const AnimationLimits limits = animationLimitsFromConfig(config);
    CHECK(limits.maxBytes == (size_t{64} << 20) && limits.maxFrames == 5);
}

// 多ページ (TIFF / ICO) とアニメーション (GIF) を返すデコーダ。
// 拡張子で振る舞いを変え、フレームごとに違う色を返して取り違えを検出できるようにする
class MultiFrameDecoder final : public IImageDecoder {
public:
    std::shared_ptr<DecodedImage> decode(const std::filesystem::path& path,
                                         std::string* = nullptr) override {
        // ICO の index 0 は「最大サイズ」= probeSequence の並びの先頭と一致させる
        if (path.extension() == ".ico") return solidImage(32, 32, 10, 255);
        return solidImage(200, 100, 10, 255);
    }

    SequenceInfo probeSequence(const std::filesystem::path& path) override {
        ++probeCount;
        SequenceInfo info;
        const std::string ext = path.extension().string();
        if (ext == ".tif") {
            info.kind = SequenceKind::Pages;
            info.frameCount = 3;
        } else if (ext == ".ico") {
            info.kind = SequenceKind::Pages;
            info.frameCount = 2;
            info.labels = {"32 x 32", "16 x 16"};
        } else if (ext == ".gif") {
            info.kind = SequenceKind::Animation;
            info.frameCount = 3;
            info.loopCount = 0;  // 無限
        }
        return info;
    }

    std::shared_ptr<DecodedImage> decodePage(const std::filesystem::path& path,
                                             uint32_t index, std::string*) override {
        ++pageCount;
        if (path.extension() == ".ico") return solidImage(16, 16, 60, 255);
        return solidImage(200, 100, static_cast<uint8_t>(20 + index), 255);
    }

    bool decodeAnimation(const std::filesystem::path& path, const AnimationLimits&,
                         ImageSequence& out, std::string*) override {
        if (path.extension() != ".gif") return false;
        ++animationCount;
        if (tooLarge) {
            out.truncated = true;  // 上限超過(App は静止画として案内する)
            return false;
        }
        out.kind = SequenceKind::Animation;
        out.loopCount = 0;
        const uint32_t delays[] = {50, 0, 200};  // 0 は既定値 (100ms) へ読み替えられる
        for (uint32_t i = 0; i < 3; ++i) {
            out.frames.push_back(FrameEntry{
                solidImage(200, 100, static_cast<uint8_t>(100 + i), 255), delays[i], {}});
        }
        return true;
    }

    bool tooLarge = false;
    int probeCount = 0;
    int pageCount = 0;
    int animationCount = 0;
};

// 現在のフレームを見分けるための色(solidImage の blue 成分)
uint8_t frameTag(const App& app) {
    return app.currentImage() ? app.currentImage()->pixels[0] : 0;
}

void testAppMultiPage() {
    MultiFrameDecoder decoder;
    ImageCache cache(decoder);
    FakeHost host;
    FakeFileSystem fileSystem;
    FakeClipboard clipboard;
    FakeEncoder encoder;
    FakeAnnotationRasterizer rasterizer;
    FakeOcrEngine ocrEngine;
    OcrService ocrService(ocrEngine);
    ScanService scanService(fileSystem);
    FakePrinter printer;
    App app(host, fileSystem, cache, clipboard, encoder, rasterizer, ocrService, printer,
            scanService);
    app.onResize(800, 600);

    std::mutex mutex;
    std::condition_variable cv;
    int decoded = 0;
    cache.setOnDecoded([&](const std::filesystem::path&) {
        std::lock_guard lock(mutex);
        ++decoded;
        cv.notify_all();
    });
    const auto pump = [&](int count) {
        std::unique_lock lock(mutex);
        CHECK(cv.wait_for(lock, std::chrono::seconds(5), [&] { return decoded >= count; }));
        lock.unlock();
        app.onDecodeCompleted();
    };

    const std::filesystem::path path = "C:/pics/doc.tif";
    fileSystem.files = {path};
    app.openPath(path);
    pump(1);  // 先頭ページのデコード完了 → 表示 → フレーム構成の調査を予約
    CHECK(frameTag(app) == 10);
    pump(2);  // 調査完了 → 3 ページあることが分かる
    CHECK(decoder.probeCount == 1);
    CHECK(app.statusBar().leftText.find("ページ 1/3") != std::string::npos);

    // 次のページ: デコードを待つ間は前のページを出したままページ番号だけ進む
    app.execute(Command::NextFrame);
    CHECK(app.statusBar().leftText.find("ページ 2/3") != std::string::npos);
    CHECK(frameTag(app) == 10);
    pump(3);
    CHECK(frameTag(app) == 21);  // 2 ページ目の絵に入れ替わった
    CHECK(decoder.pageCount == 1);

    // 前のページへ戻る。デコード済みなので待たずに切り替わる
    app.execute(Command::PrevFrame);
    CHECK(frameTag(app) == 10);
    CHECK(decoder.pageCount == 1);  // 再デコードは起きない
    app.execute(Command::PrevFrame);
    CHECK(app.statusBar().leftText == "最初のページです");
    app.onTimer();

    // 末尾では折り返さない(ファイル遷移もしない)
    app.execute(Command::NextFrame);  // デコード済みのページなので待たずに切り替わる
    CHECK(frameTag(app) == 21);
    app.execute(Command::NextFrame);  // 3 ページ目は未デコード
    pump(4);
    CHECK(frameTag(app) == 22);
    CHECK(decoder.pageCount == 2);
    CHECK(app.statusBar().leftText.find("ページ 3/3") != std::string::npos);
    app.execute(Command::NextFrame);
    CHECK(app.statusBar().leftText == "最後のページです");
    app.onTimer();

    // 多ページの上書き保存は断る(表示中の 1 枚で潰すと残りが消える)。
    // 縮小画像と同じく、確認ダイアログを出す前に断ること
    app.execute(Command::SaveImage);
    CHECK(host.confirmCount == 0);
    CHECK(encoder.encodeCount == 0);
    CHECK(app.statusBar().leftText.find("ページが 3 枚あります") != std::string::npos);
    app.onTimer();

    // アニメーションではないので再生できない
    app.execute(Command::TogglePlay);
    CHECK(app.statusBar().leftText == "この画像はアニメーションではありません");
    CHECK(host.lastFrameTimerMs == 0);
}

void testAppIcoSizes() {
    MultiFrameDecoder decoder;
    ImageCache cache(decoder);
    FakeHost host;
    FakeFileSystem fileSystem;
    FakeClipboard clipboard;
    FakeEncoder encoder;
    FakeAnnotationRasterizer rasterizer;
    FakeOcrEngine ocrEngine;
    OcrService ocrService(ocrEngine);
    ScanService scanService(fileSystem);
    FakePrinter printer;
    App app(host, fileSystem, cache, clipboard, encoder, rasterizer, ocrService, printer,
            scanService);
    app.onResize(800, 600);

    std::mutex mutex;
    std::condition_variable cv;
    int decoded = 0;
    cache.setOnDecoded([&](const std::filesystem::path&) {
        std::lock_guard lock(mutex);
        ++decoded;
        cv.notify_all();
    });
    const auto pump = [&](int count) {
        std::unique_lock lock(mutex);
        CHECK(cv.wait_for(lock, std::chrono::seconds(5), [&] { return decoded >= count; }));
        lock.unlock();
        app.onDecodeCompleted();
    };

    const std::filesystem::path path = "C:/pics/app.ico";
    fileSystem.files = {path};
    app.openPath(path);
    pump(1);
    pump(2);
    // index 0 は最大サイズ(ファイル内の先頭ではない)。サイズを表示名に出す
    CHECK(app.currentImage() && app.currentImage()->width == 32);
    CHECK(app.statusBar().leftText.find("ページ 1/2 (32 x 32)") != std::string::npos);

    // 大きさの違うページへ移ったらフィットし直す(アニメーションと違い canvas が変わる)
    app.execute(Command::NextFrame);
    pump(3);
    CHECK(app.currentImage() && app.currentImage()->width == 16);
    CHECK(app.statusBar().leftText.find("ページ 2/2 (16 x 16)") != std::string::npos);
}

void testAppAnimation() {
    MultiFrameDecoder decoder;
    ImageCache cache(decoder);
    FakeHost host;
    FakeFileSystem fileSystem;
    FakeClipboard clipboard;
    FakeEncoder encoder;
    FakeAnnotationRasterizer rasterizer;
    FakeOcrEngine ocrEngine;
    OcrService ocrService(ocrEngine);
    ScanService scanService(fileSystem);
    FakePrinter printer;
    App app(host, fileSystem, cache, clipboard, encoder, rasterizer, ocrService, printer,
            scanService);
    app.onResize(800, 600);

    std::mutex mutex;
    std::condition_variable cv;
    int decoded = 0;
    cache.setOnDecoded([&](const std::filesystem::path&) {
        std::lock_guard lock(mutex);
        ++decoded;
        cv.notify_all();
    });
    const auto pump = [&](int count) {
        std::unique_lock lock(mutex);
        CHECK(cv.wait_for(lock, std::chrono::seconds(5), [&] { return decoded >= count; }));
        lock.unlock();
        app.onDecodeCompleted();
    };

    const std::filesystem::path path = "C:/pics/anim.gif";
    fileSystem.files = {path};
    app.openPath(path);
    pump(1);  // 先頭フレーム。この時点ではまだ静止画に見える
    CHECK(app.statusBar().leftText.find("フレーム") == std::string::npos);
    CHECK(host.lastFrameTimerMs == 0);

    pump(2);  // 調査 → 全フレーム展開まで終わる(調査だけでは通知しない)
    CHECK(decoder.animationCount == 1);
    CHECK(app.statusBar().leftText.find("フレーム 1/3 再生中") != std::string::npos);
    CHECK(frameTag(app) == 100);  // 展開後の先頭フレームへ差し替わっている
    CHECK(host.lastFrameTimerMs == 50);  // 既定で自動再生。1 枚目の表示時間

    // タイマー満了でコマが進み、次の時間が張り直される
    app.onFrameTimer();
    CHECK(frameTag(app) == 101);
    CHECK(host.lastFrameTimerMs == 100);  // 遅延 0 は既定値へ読み替える
    CHECK(app.statusBar().leftText.find("フレーム 2/3") != std::string::npos);
    app.onFrameTimer();
    CHECK(frameTag(app) == 102);
    CHECK(host.lastFrameTimerMs == 200);
    app.onFrameTimer();  // 無限ループなので先頭へ戻る
    CHECK(frameTag(app) == 100);
    CHECK(host.lastFrameTimerMs == 50);

    // Space で一時停止 → タイマーが止まる
    CHECK(app.onKey({KeyCode::Space}));
    CHECK(host.lastFrameTimerMs == 0);
    CHECK(app.statusBar().leftText.find("フレーム 1/3 停止中") != std::string::npos);
    app.onFrameTimer();  // 止まっている間の満了は無視する
    CHECK(frameTag(app) == 100);

    // 手動送りは折り返さず、送った先で止まったまま
    app.execute(Command::NextFrame);
    CHECK(frameTag(app) == 101);
    CHECK(host.lastFrameTimerMs == 0);

    // 再開 → 現在のフレームの時間から
    CHECK(app.onKey({KeyCode::Space}));
    CHECK(host.lastFrameTimerMs == 100);

    // 編集を始めたら再生は止まり、そのフレームの静止画になる
    app.onMouseDown(MouseButton::Right, {380, 280});
    app.onMouseMove({430, 320});
    app.onMouseUp(MouseButton::Right, {430, 320});
    CHECK(app.annotations().specs->size() == 1);
    CHECK(host.lastFrameTimerMs == 0);
    app.execute(Command::TogglePlay);
    CHECK(app.statusBar().leftText.find("編集中は再生できません") == 0);
    app.onTimer();

    // 複数フレームの画像も上書き保存は断る
    app.execute(Command::SaveImage);
    CHECK(host.confirmCount == 0);
    CHECK(encoder.encodeCount == 0);
    CHECK(app.statusBar().leftText.find("フレームが 3 枚あります") != std::string::npos);
}

void testAppAnimationTooLarge() {
    MultiFrameDecoder decoder;
    decoder.tooLarge = true;
    ImageCache cache(decoder);
    FakeHost host;
    FakeFileSystem fileSystem;
    FakeClipboard clipboard;
    FakeEncoder encoder;
    FakeAnnotationRasterizer rasterizer;
    FakeOcrEngine ocrEngine;
    OcrService ocrService(ocrEngine);
    ScanService scanService(fileSystem);
    FakePrinter printer;
    App app(host, fileSystem, cache, clipboard, encoder, rasterizer, ocrService, printer,
            scanService);
    app.onResize(800, 600);

    std::mutex mutex;
    std::condition_variable cv;
    int decoded = 0;
    cache.setOnDecoded([&](const std::filesystem::path&) {
        std::lock_guard lock(mutex);
        ++decoded;
        cv.notify_all();
    });
    const auto pump = [&](int count) {
        std::unique_lock lock(mutex);
        CHECK(cv.wait_for(lock, std::chrono::seconds(5), [&] { return decoded >= count; }));
        lock.unlock();
        app.onDecodeCompleted();
    };

    const std::filesystem::path path = "C:/pics/huge.gif";
    fileSystem.files = {path};
    app.openPath(path);
    pump(1);
    pump(2);
    // 途中まで再生すると不具合にしか見えないので、静止画として理由を案内する
    CHECK(app.statusBar().leftText.find("静止画として表示") != std::string::npos);
    CHECK(host.lastFrameTimerMs == 0);
    app.onTimer();
    CHECK(app.statusBar().leftText.find("フレーム") == std::string::npos);
    // 1 枚しかないので上書き保存は通常どおり可能(拡張子が対応していれば)
    app.execute(Command::SaveImage);
    CHECK(app.statusBar().leftText.find("フレームが") == std::string::npos);
}

} // namespace

void testSortOrder() {
    const auto make = [](const std::string& name, const int64_t tick, const uint64_t size) {
        FileEntry entry;
        entry.path = "C:/pics/" + name;
        entry.relative = name;
        entry.lastWriteTick = tick;
        entry.sizeBytes = size;
        return entry;
    };
    // IFileSystem の契約どおり名前昇順で渡す。同値の tie-break はこの並びで決まる
    const std::vector<FileEntry> entries = {
        make("a.png", 30, 100),
        make("b.jpg", 10, 300),
        make("c.png", 20, 100),
        make("d.jpg", 10, 200),
    };
    const auto names = [&entries](const std::vector<size_t>& order) {
        std::string joined;
        for (const size_t i : order) {
            if (!joined.empty()) joined += ',';
            joined += pathToUtf8(entries[i].path.filename());
        }
        return joined;
    };

    CHECK(names(sortedOrder(entries, {SortKey::Name, false})) == "a.png,b.jpg,c.png,d.jpg");
    CHECK(names(sortedOrder(entries, {SortKey::Name, true})) == "d.jpg,c.png,b.jpg,a.png");

    // 更新日時。同じ時刻の b と d は名前昇順のまま並ぶ
    CHECK(names(sortedOrder(entries, {SortKey::Date, false})) == "b.jpg,d.jpg,c.png,a.png");
    // 降順でも同値の中は名前昇順のまま(std::reverse で実装してはならない理由)
    CHECK(names(sortedOrder(entries, {SortKey::Date, true})) == "a.png,c.png,b.jpg,d.jpg");

    CHECK(names(sortedOrder(entries, {SortKey::Size, false})) == "a.png,c.png,d.jpg,b.jpg");
    CHECK(names(sortedOrder(entries, {SortKey::Size, true})) == "b.jpg,d.jpg,a.png,c.png");

    CHECK(names(sortedOrder(entries, {SortKey::Extension, false})) == "b.jpg,d.jpg,a.png,c.png");
    CHECK(names(sortedOrder(entries, {SortKey::Extension, true})) == "a.png,c.png,b.jpg,d.jpg");

    // 拡張子の大文字小文字は区別しない
    const std::vector<FileEntry> mixed = {make("x.JPG", 0, 0), make("y.png", 0, 0),
                                          make("z.jpg", 0, 0)};
    std::string mixedNames;
    for (const size_t i : sortedOrder(mixed, {SortKey::Extension, false})) {
        if (!mixedNames.empty()) mixedNames += ',';
        mixedNames += pathToUtf8(mixed[i].path.filename());
    }
    CHECK(mixedNames == "x.JPG,z.jpg,y.png");

    CHECK(sortedOrder({}, {SortKey::Date, true}).empty());

    // ini 表記の往復と巡回
    CHECK(sortKeyIniName(SortKey::Extension) == "ext");
    CHECK(sortKeyFromIniName(" Date ") == SortKey::Date);
    CHECK(!sortKeyFromIniName("mtime").has_value());
    CHECK(nextSortKey(SortKey::Name) == SortKey::Date);
    CHECK(nextSortKey(SortKey::Extension) == SortKey::Name);
    CHECK(sortOrderLabel({SortKey::Date, true}) == "更新日時 (新しい順)");
}

void testResizeImage() {
    // 2x2。B チャンネルだけ値を変え、G/R/A は一定にして混ざり方を見る
    DecodedImage img;
    img.width = 2;
    img.height = 2;
    const uint8_t blues[4] = {0, 100, 200, 255};
    for (const uint8_t b : blues) {
        img.pixels.insert(img.pixels.end(), {b, 10, 20, 255});
    }

    CHECK(resizeImage(img, 0, 4) == nullptr);
    CHECK(resizeImage(img, 4, 0) == nullptr);
    CHECK(resizeImage(DecodedImage{}, 4, 4) == nullptr);
    CHECK(resizeImage(img, kMaxResizeDimension + 1, 4) == nullptr);
    CHECK(resizeImage(img, 4, kMaxResizeDimension + 1) == nullptr);

    // 等倍の指定は恒等(半径 1 の三角フィルタは中心以外の重みが 0 になる)
    const auto same = resizeImage(img, 2, 2);
    CHECK(same != nullptr);
    CHECK(same->pixels == img.pixels);

    // 2x2 → 1x1 は 4 画素の平均。固定小数の丸めで ±2 程度ずれる
    const auto one = resizeImage(img, 1, 1);
    CHECK(one != nullptr);
    CHECK(one->width == 1 && one->height == 1);
    CHECK(std::abs(static_cast<int>(one->pixels[0]) - (0 + 100 + 200 + 255) / 4) <= 2);
    CHECK(one->pixels[1] == 10 && one->pixels[2] == 20 && one->pixels[3] == 255);

    // 拡大しても四隅の色は元のまま(端の外側は端の画素へ畳み込む)
    const auto up = resizeImage(img, 4, 4);
    CHECK(up != nullptr);
    CHECK(up->width == 4 && up->height == 4);
    CHECK(up->pixels[0] == 0);                  // 左上 = src(0,0)
    CHECK(up->pixels[(3 * 4 + 3) * 4] == 255);  // 右下 = src(1,1)
    CHECK(up->pixels[3] == 255);                // アルファは保たれる

    // 一様な画像はどう変えても値が変わらない(重みの合計が 1 になっていること)
    DecodedImage flat;
    flat.width = 7;
    flat.height = 3;
    flat.pixels.assign(7 * 3 * 4, 128);
    const auto stretched = resizeImage(flat, 3, 11);
    CHECK(stretched != nullptr);
    CHECK(stretched->pixels == std::vector<uint8_t>(3 * 11 * 4, 128));

    // 取り込み時に縮小された画像は、リサイズしても上書き保存を拒む印を保つ
    CHECK(!up->downscaled());
    DecodedImage shrunk = img;
    shrunk.sourceWidth = 8;
    shrunk.sourceHeight = 8;
    const auto resizedShrunk = resizeImage(shrunk, 4, 4);
    CHECK(resizedShrunk != nullptr);
    CHECK(resizedShrunk->downscaled());
    CHECK(resizedShrunk->sourceWidth == 8 && resizedShrunk->sourceHeight == 8);
}

void testScaleAnnotation() {
    AnnotationSpec spec;
    spec.p1 = {10, 20};
    spec.p2 = {30, 60};
    spec.points = {{10, 20}, {30, 60}};
    spec.strokeWidth = 4;
    spec.fontSize = 20;
    spec.borderWidth = 2;
    spec.angleDeg = 30;

    scaleAnnotation(spec, 2.0f, 0.5f);
    CHECK(nearly(spec.p1.x, 20) && nearly(spec.p1.y, 10));
    CHECK(nearly(spec.p2.x, 60) && nearly(spec.p2.y, 30));
    CHECK(nearly(spec.points[1].x, 60) && nearly(spec.points[1].y, 30));
    // 1 次元の量は小さいほうの倍率(ここでは 0.5)に合わせる
    CHECK(nearly(spec.strokeWidth, 2));
    CHECK(nearly(spec.fontSize, 10));
    CHECK(nearly(spec.borderWidth, 1));
    CHECK(nearly(spec.angleDeg, 30));  // 回転角は変えない
}

void testAppSortOrder() {
    FakeDecoder decoder;
    ImageCache cache(decoder);
    FakeHost host;
    FakeFileSystem fileSystem;
    FakeClipboard clipboard;
    FakeEncoder encoder;
    FakeAnnotationRasterizer rasterizer;
    FakeOcrEngine ocrEngine;
    OcrService ocrService(ocrEngine);
    ScanService scanService(fileSystem);
    FakePrinter printer;
    App app(host, fileSystem, cache, clipboard, encoder, rasterizer, ocrService, printer,
            scanService);
    app.onResize(800, 600);

    std::mutex mutex;
    std::condition_variable cv;
    bool decoded = false;
    cache.setOnDecoded([&](const std::filesystem::path&) {
        std::lock_guard lock(mutex);
        decoded = true;
        cv.notify_all();
    });

    fileSystem.files = {"C:/pics/a.png", "C:/pics/b.png", "C:/pics/c.png"};
    fileSystem.ticks = {10, 20, 30};  // a が最も古い = 「新しい順」は名前の逆順になる
    app.openPath(fileSystem.files[0]);
    {
        std::unique_lock lock(mutex);
        CHECK(cv.wait_for(lock, std::chrono::seconds(5), [&] { return decoded; }));
    }
    app.onDecodeCompleted();
    app.execute(Command::ToggleSidebar);

    const auto labels = [&app] {
        std::string joined;
        for (const auto& item : app.sidebar().items) {
            if (!joined.empty()) joined += ',';
            joined += item.text;
        }
        return joined;
    };
    const auto currentIndex = [&app] {
        const SidebarView view = app.sidebar();
        for (size_t i = 0; i < view.items.size(); ++i) {
            if (view.items[i].current) return i;
        }
        return view.items.size();
    };
    CHECK(labels() == "a.png,b.png,c.png");
    CHECK(currentIndex() == 0);

    // 並び替えても表示中の画像は変わらない(一覧内の位置だけが動く)
    app.execute(Command::SortByDate);  // 別のキーへ移るとき、日時だけ「新しい順」が既定
    CHECK(labels() == "c.png,b.png,a.png");
    CHECK(currentIndex() == 2);
    CHECK(app.currentImage() != nullptr);

    app.execute(Command::SortByDate);  // 同じキーをもう一度で昇順 / 降順が反転する
    CHECK(labels() == "a.png,b.png,c.png");
    CHECK(currentIndex() == 0);

    app.execute(Command::SortByName);
    CHECK(labels() == "a.png,b.png,c.png");
    CHECK(currentIndex() == 0);
    app.execute(Command::ToggleSortDescending);
    CHECK(labels() == "c.png,b.png,a.png");
    CHECK(currentIndex() == 2);
}

void testAppRecursive() {
    FakeDecoder decoder;
    ImageCache cache(decoder);
    FakeHost host;
    FakeFileSystem fileSystem;
    FakeClipboard clipboard;
    FakeEncoder encoder;
    FakeAnnotationRasterizer rasterizer;
    FakeOcrEngine ocrEngine;
    OcrService ocrService(ocrEngine);
    ScanService scanService(fileSystem);
    FakePrinter printer;
    App app(host, fileSystem, cache, clipboard, encoder, rasterizer, ocrService, printer,
            scanService);
    app.onResize(800, 600);

    std::mutex mutex;
    std::condition_variable cv;
    bool decoded = false;
    bool scanned = false;
    cache.setOnDecoded([&](const std::filesystem::path&) {
        std::lock_guard lock(mutex);
        decoded = true;
        cv.notify_all();
    });
    scanService.setOnCompleted([&] {
        std::lock_guard lock(mutex);
        scanned = true;
        cv.notify_all();
    });
    const auto waitFor = [&](bool& flag) {
        std::unique_lock lock(mutex);
        const bool ok = cv.wait_for(lock, std::chrono::seconds(5), [&] { return flag; });
        flag = false;
        return ok;
    };

    fileSystem.files = {"C:/pics/a.png", "C:/pics/b.png"};
    // 再帰時は「親フォルダ → ファイル名」の順(直下が先、サブフォルダが後)
    fileSystem.recursiveFiles = {"C:/pics/a.png", "C:/pics/b.png", "C:/pics/sub/c.png"};
    app.openPath(fileSystem.files[0]);
    CHECK(waitFor(decoded));
    app.onDecodeCompleted();
    app.execute(Command::ToggleSidebar);
    CHECK(app.sidebar().items.size() == 2);

    // サブフォルダの走査はワーカーで走り、完了通知を受けてから一覧が入れ替わる
    app.execute(Command::ToggleRecursive);
    CHECK(app.sidebar().items.size() == 2);  // 走査中は直下のままで、待たされない
    CHECK(waitFor(scanned));
    app.onScanCompleted();
    CHECK(app.sidebar().items.size() == 3);
    // 再帰中はファイル名だけでは区別できないので相対パスを出す
    const std::string subLabel = app.sidebar().items[2].text;
    CHECK(subLabel.find("c.png") != std::string::npos);
    CHECK(subLabel != "c.png");
    // 表示中の画像は変わらない
    CHECK(app.sidebar().items[0].current);
    CHECK(app.currentImage() != nullptr);

    // サブフォルダの画像を表示している状態で再帰を切ると、その画像は一覧から消えるので
    // 現在位置の画像へ切り替わる(表示中の画像が残るときは切り替えない)
    app.execute(Command::LastImage);
    CHECK(waitFor(decoded));
    app.onDecodeCompleted();
    CHECK(app.sidebar().items[2].current);

    // 戻すほうは同期で済ませる(走査ワーカーを起こさない)
    app.execute(Command::ToggleRecursive);
    CHECK(app.sidebar().items.size() == 2);
    CHECK(app.sidebar().items[0].text == "a.png");
    CHECK(app.sidebar().items[0].current);
}

void testAppResize() {
    FakeDecoder decoder;
    ImageCache cache(decoder);
    FakeHost host;
    FakeFileSystem fileSystem;
    FakeClipboard clipboard;
    FakeEncoder encoder;
    FakeAnnotationRasterizer rasterizer;
    FakeOcrEngine ocrEngine;
    OcrService ocrService(ocrEngine);
    ScanService scanService(fileSystem);
    FakePrinter printer;
    App app(host, fileSystem, cache, clipboard, encoder, rasterizer, ocrService, printer,
            scanService);
    app.onResize(800, 600);

    // 8x8 の貼り付け画像に矩形注釈 (0,0)-(4,4) を置く(画像左上はスクリーン (396,283))
    auto source = std::make_shared<DecodedImage>();
    source->width = 8;
    source->height = 8;
    source->pixels.resize(8 * 8 * 4, 200);
    clipboard.pasteImage = source;
    app.execute(Command::PasteImage);
    app.onMouseDown(MouseButton::Right, {396, 283});
    app.onMouseUp(MouseButton::Right, {400, 287});
    CHECK(app.annotations().specs->size() == 1);
    const AnnotationSpec before = app.annotations().specs->front();

    // メニューの「倍率 → 50%」を選んだのと同じ経路(host が末端項目の index を返す)。
    // 倍率は 200 / 150 / 75 / 50 / 25 の順なので 4 番目
    host.menuChoice = 3;
    app.execute(Command::ResizeImage);
    CHECK(app.currentImage()->width == 4);
    CHECK(app.currentImage()->height == 4);
    // 注釈は焼き込まず、同じ倍率で座標が追従する
    CHECK(app.annotations().specs->size() == 1);
    CHECK(nearly(app.annotations().specs->front().p2.x, before.p2.x * 0.5f));
    CHECK(nearly(app.annotations().specs->front().p2.y, before.p2.y * 0.5f));

    // 長辺の指定でも同じ経路を通る(倍率5 + 長辺の 1920 は index 5+2=7)
    host.menuChoice = 7;
    app.execute(Command::ResizeImage);
    CHECK(app.currentImage()->width == 1920);
    CHECK(app.currentImage()->height == 1920);

    // 取り消せる(トリミングと同じ破壊的編集)
    app.execute(Command::Undo);
    CHECK(app.currentImage()->width == 4);
    app.execute(Command::Undo);
    CHECK(app.currentImage()->width == 8);
    CHECK(nearly(app.annotations().specs->front().p2.x, before.p2.x));
}

int main() {
    std::cout << std::unitbuf;  // 途中で落ちても FAIL の出力を失わない
    testUnicode();
    testUtf16Offsets();
    testNaturalCompare();
    testTextEditBuffer();
    testTextStyleRuns();
    testTextEditBufferStyles();
    testMatrix();
    testViewportFit();
    testViewportZoomAt();
    testKeymap();
    testChordToString();
    testMousemap();
    testNavArrows();
    testHelpLines();
    testConfig();
    testDib();
    testPixelConvert();
    testApplyExifOrientation();
    testReadExifOrientation();
    testImageList();
    testImageCache();
    testImageCacheLimits();
    testAppSlowDecode();
    testAppClipboard();
    testAppStatusBar();
    testAppPasteWithoutFolder();
    testAppWheelHorizontal();
    testAppPasteSave();
    testAppSaveOverwrite();
    testAppSaveDownscaled();
    testAppSaveColorConverted();
    testPrintLayout();
    testAppPrint();
    testImageCacheColorRefine();
    testAppAdoptsRefinedImage();
    testDownscaleToFit();
    testResizeImage();
    testScaleAnnotation();
    testSortOrder();
    testSidebarState();
    testPointerState();
    testAppSidebar();
    testAppSidebarResize();
    testAppHelpSidebar();
    testAppHelpHint();
    testEditFunctions();
    testEditHistory();
    testAnnotationGeometry();
    testPenGeometry();
    testPastedImageGeometry();
    testAppAnnotationObjects();
    testAppEdit();
    testAppTextEditing();
    testAppTextStyles();
    testAppFontFamily();
    testAppPasteObject();
    testAppPenTools();
    testAnnotationSizeIsImageBased();
    testAppSwapMouseButtons();
    testOcrText();
    testOcrRetryUpscale();
    testFlattenOnBackground();
    testOcrService();
    testAppOcr();
    testAppSortOrder();
    testAppRecursive();
    testAppResize();
    testAnimationCore();
    testAppMultiPage();
    testAppIcoSizes();
    testAppAnimation();
    testAppAnimationTooLarge();

    if (g_failures == 0) {
        std::cout << "all tests passed\n";
        return 0;
    }
    std::cout << g_failures << " check(s) failed\n";
    return 1;
}
