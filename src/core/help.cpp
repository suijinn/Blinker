#include "core/help.h"

#include <format>
#include <string_view>

namespace blinker {
namespace {

// 操作名とキーの間隔。サイドバーの幅は等幅ではないため、桁揃えはせず空白 2 つで区切る
constexpr std::string_view kGap = "  ";

} // namespace

std::string keysLabel(const Keymap& keymap, const Command cmd) {
    std::string result;
    for (const KeyChord& chord : keymap.chordsFor(cmd)) {
        const std::string text = Keymap::chordToString(chord);
        if (text.empty()) continue;
        if (!result.empty()) result += ' ';
        result += text;
    }
    return result;
}

std::vector<HelpLine> buildHelpLines(const Keymap& keymap, const bool swapMouseButtons) {
    std::vector<HelpLine> lines;

    // 見出しは中身が 1 行でも出てから追加する(ini で全部外された節を空のまま残さない)
    std::string_view pendingHeader;
    const auto header = [&pendingHeader](std::string_view title) { pendingHeader = title; };
    const auto flushHeader = [&lines, &pendingHeader] {
        if (pendingHeader.empty()) return;
        if (!lines.empty()) lines.push_back({});  // 節の間の空行
        lines.push_back({std::string(pendingHeader), true});
        pendingHeader = {};
    };
    // キーが割り当てられていない操作は行ごと出さない(既定の tool_* など)
    const auto row = [&](const Command cmd, const std::string_view label) {
        const std::string keys = keysLabel(keymap, cmd);
        if (keys.empty()) return;
        flushHeader();
        lines.push_back({std::format("{}{}{}", label, kGap, keys), false});
    };
    // キーバインドを持たない操作(マウスなど)の固定行
    const auto text = [&](const std::string_view label, const std::string_view keys) {
        flushHeader();
        lines.push_back({std::format("{}{}{}", label, kGap, keys), false});
    };

    header("表示");
    row(Command::NextImage, "次の画像");
    row(Command::PrevImage, "前の画像");
    row(Command::FirstImage, "先頭の画像");
    row(Command::LastImage, "末尾の画像");
    row(Command::ZoomIn, "拡大");
    row(Command::ZoomOut, "縮小");
    row(Command::ZoomFit, "ウィンドウにフィット");
    row(Command::ZoomActual, "等倍表示");
    row(Command::PanLeft, "左へスクロール");
    row(Command::PanRight, "右へスクロール");
    row(Command::PanUp, "上へスクロール");
    row(Command::PanDown, "下へスクロール");
    row(Command::RotateCW, "右 90 度回転");
    row(Command::RotateCCW, "左 90 度回転");
    row(Command::ToggleFullscreen, "フルスクリーン");

    header("ファイル");
    row(Command::OpenFile, "開く");
    row(Command::SaveImageAs, "名前を付けて保存");
    row(Command::CopyImage, "画像をコピー");
    row(Command::CopyPath, "パスをコピー");
    row(Command::CopyFile, "ファイルをコピー");
    row(Command::CopyOcrText, "文字を認識してコピー");
    row(Command::PasteImage, "貼り付け");

    header("編集");
    row(Command::Undo, "元に戻す");
    row(Command::Redo, "やり直す");
    row(Command::DeleteAnnotation, "選択中の図形・テキストを削除");
    row(Command::SelectToolCrop, "トリミングツール");
    row(Command::SelectToolOcr, "文字認識ツール (範囲指定)");
    row(Command::SelectToolRect, "矩形ツール");
    row(Command::SelectToolEllipse, "楕円ツール");
    row(Command::SelectToolArrow, "矢印ツール");
    row(Command::SelectToolLine, "直線ツール");
    row(Command::SelectToolPen, "ペン (手書き) ツール");
    row(Command::SelectToolMarker, "マーカーツール");
    row(Command::SelectToolNumber, "連番マーカーツール");
    row(Command::SelectToolText, "テキストツール");
    // Ctrl+B は App が横取りするため Command を持たない(キー変更もできない)
    text("選択中のテキストを太字", "Ctrl+B");

    header("画面");
    row(Command::ToggleSidebar, "ファイル名一覧");
    row(Command::ToggleStatusBar, "ステータスバー");
    row(Command::ToggleHelp, "この操作一覧");

    header("終了");
    row(Command::Escape, "選択解除 / 全画面解除 / 終了");
    row(Command::Quit, "終了");

    // マウス操作は Command を経由しないため固定文言。編集ドラッグの中身は
    // 現在のツール(ステータスバーに出る)で決まる。パンと編集のボタンは
    // [mouse] swap_buttons で入れ替わるので、実際に効くほうを出す
    // (メニューは常に右クリック、オブジェクトを掴む操作は常に左で、どちらも
    //  入れ替えの対象外なので固定文言のまま)
    const std::string_view panDrag = swapMouseButtons ? "右ドラッグ" : "左ドラッグ";
    const std::string_view editDrag = swapMouseButtons ? "左ドラッグ" : "右ドラッグ";
    const std::string_view editDragShift = swapMouseButtons ? "Shift+左ドラッグ" : "Shift+右ドラッグ";
    header("マウス");
    text("拡大 / 縮小", "ホイール");
    text("スクロール", panDrag);
    text("現在のツールを実行", editDrag);
    text("正方形 / 真円で描く", editDragShift);
    text("直線・矢印・手書きをまっすぐ", editDragShift);
    text("ツール・書式メニュー", "余白で右クリック");
    text("図形・テキストを選択", "左クリック");
    text("選択中のオブジェクトを移動", "左ドラッグ");
    text("オブジェクトのメニュー", "図形の上で右クリック");
    text("テキストを再編集", "ダブルクリック");

    header("設定");
    flushHeader();
    lines.push_back({"blinker.exe と同じ場所の", false});
    lines.push_back({"blinker.ini でキーを変更できます", false});
    lines.push_back({"(上のキー表記をそのまま書けます)", false});

    return lines;
}

} // namespace blinker
