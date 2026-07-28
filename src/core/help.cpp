#include "core/help.h"

#include <array>
#include <format>
#include <string_view>

namespace blinker {
namespace {

// 操作名とキーの間隔。サイドバーの幅は等幅ではないため、桁揃えはせず空白 2 つで区切る
constexpr std::string_view kGap = "  ";

struct CommandLabel {
    Command cmd;
    std::string_view label;
};

// コマンドの表示名。並びは操作一覧に出す順(「マウス」の節もこの順で走査する)。
// 表示名の正はここだけにする(キーの節とマウスの節で文言がずれないように)
constexpr std::array kCommandLabels = {
    CommandLabel{Command::NextImage, "次の画像"},
    CommandLabel{Command::PrevImage, "前の画像"},
    CommandLabel{Command::FirstImage, "先頭の画像"},
    CommandLabel{Command::LastImage, "末尾の画像"},
    CommandLabel{Command::ZoomIn, "拡大"},
    CommandLabel{Command::ZoomOut, "縮小"},
    CommandLabel{Command::ZoomFit, "ウィンドウにフィット"},
    CommandLabel{Command::ZoomActual, "等倍表示"},
    CommandLabel{Command::PanLeft, "左へスクロール"},
    CommandLabel{Command::PanRight, "右へスクロール"},
    CommandLabel{Command::PanUp, "上へスクロール"},
    CommandLabel{Command::PanDown, "下へスクロール"},
    CommandLabel{Command::RotateCW, "右 90 度回転"},
    CommandLabel{Command::RotateCCW, "左 90 度回転"},
    CommandLabel{Command::ToggleFullscreen, "フルスクリーン"},
    CommandLabel{Command::OpenFile, "開く"},
    CommandLabel{Command::SaveImage, "上書き保存"},
    CommandLabel{Command::SaveImageAs, "名前を付けて保存"},
    CommandLabel{Command::PrintImage, "印刷"},
    CommandLabel{Command::ResizeImage, "画像をリサイズ"},
    CommandLabel{Command::SortByName, "名前順に並べ替え"},
    CommandLabel{Command::SortByDate, "更新日時順に並べ替え"},
    CommandLabel{Command::SortBySize, "サイズ順に並べ替え"},
    CommandLabel{Command::SortByExtension, "種類順に並べ替え"},
    CommandLabel{Command::ToggleSortDescending, "昇順 / 降順を反転"},
    CommandLabel{Command::CycleSortKey, "並び替えキーを切り替え"},
    CommandLabel{Command::ToggleRecursive, "サブフォルダを含める"},
    CommandLabel{Command::CopyImage, "画像をコピー"},
    CommandLabel{Command::CopyPath, "パスをコピー"},
    CommandLabel{Command::CopyFile, "ファイルをコピー"},
    CommandLabel{Command::CopyOcrText, "文字を認識してコピー"},
    CommandLabel{Command::PasteImage, "貼り付け"},
    CommandLabel{Command::Undo, "元に戻す"},
    CommandLabel{Command::Redo, "やり直す"},
    CommandLabel{Command::DeleteAnnotation, "選択中の図形・テキストを削除"},
    CommandLabel{Command::SelectToolCrop, "トリミングツール"},
    CommandLabel{Command::SelectToolOcr, "文字認識ツール (範囲指定)"},
    CommandLabel{Command::SelectToolRect, "矩形ツール"},
    CommandLabel{Command::SelectToolEllipse, "楕円ツール"},
    CommandLabel{Command::SelectToolArrow, "矢印ツール"},
    CommandLabel{Command::SelectToolLine, "直線ツール"},
    CommandLabel{Command::SelectToolPen, "ペン (手書き) ツール"},
    CommandLabel{Command::SelectToolMarker, "マーカーツール"},
    CommandLabel{Command::SelectToolNumber, "連番マーカーツール"},
    CommandLabel{Command::SelectToolText, "テキストツール"},
    CommandLabel{Command::ToggleSidebar, "ファイル名一覧"},
    CommandLabel{Command::ToggleStatusBar, "ステータスバー"},
    CommandLabel{Command::ToggleHelp, "この操作一覧"},
    CommandLabel{Command::Escape, "選択解除 / 全画面解除 / 終了"},
    CommandLabel{Command::Quit, "終了"},
};

std::string_view commandLabel(const Command cmd) {
    for (const auto& e : kCommandLabels) {
        if (e.cmd == cmd) return e.label;
    }
    return {};
}

// ホイールでのズームの表記。ズームは割り当てではなく「未割り当てのホイールの
// 既定動作」なので、実際に空いているほうを出す(両方 [mouse] で埋められていたら
// ホイールではズームできないため行ごと出さない)
std::string_view zoomWheelLabel(const Mousemap& mousemap) {
    const auto bound = [&mousemap](const bool ctrl) {
        return mousemap.find({MouseInput::WheelUp, ctrl}) != Command::None ||
               mousemap.find({MouseInput::WheelDown, ctrl}) != Command::None;
    };
    if (!bound(false)) return "ホイール";
    if (!bound(true)) return "Ctrl+ホイール";
    return {};
}

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

std::string mouseLabel(const Mousemap& mousemap, const Command cmd) {
    std::string result;
    for (const MouseChord& chord : mousemap.chordsFor(cmd)) {
        const std::string text = Mousemap::chordToDisplayString(chord);
        if (text.empty()) continue;
        if (!result.empty()) result += ' ';
        result += text;
    }
    return result;
}

std::vector<HelpLine> buildHelpLines(const Keymap& keymap, const Mousemap& mousemap,
                                     const bool swapMouseButtons) {
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
    // キーバインドを持たない操作(マウスなど)の固定行
    const auto text = [&](const std::string_view label, const std::string_view keys) {
        if (label.empty() || keys.empty()) return;
        flushHeader();
        lines.push_back({std::format("{}{}{}", label, kGap, keys), false});
    };
    // キーが割り当てられていない操作は行ごと出さない(既定の tool_* など)
    const auto row = [&](const Command cmd) {
        text(commandLabel(cmd), keysLabel(keymap, cmd));
    };

    header("表示");
    row(Command::NextImage);
    row(Command::PrevImage);
    row(Command::FirstImage);
    row(Command::LastImage);
    row(Command::ZoomIn);
    row(Command::ZoomOut);
    row(Command::ZoomFit);
    row(Command::ZoomActual);
    row(Command::PanLeft);
    row(Command::PanRight);
    row(Command::PanUp);
    row(Command::PanDown);
    row(Command::RotateCW);
    row(Command::RotateCCW);
    row(Command::ToggleFullscreen);

    header("ファイル");
    row(Command::OpenFile);
    row(Command::SaveImage);
    row(Command::SaveImageAs);
    row(Command::PrintImage);
    row(Command::CopyImage);
    row(Command::CopyPath);
    row(Command::CopyFile);
    row(Command::CopyOcrText);
    row(Command::PasteImage);

    header("一覧");
    row(Command::SortByName);
    row(Command::SortByDate);
    row(Command::SortBySize);
    row(Command::SortByExtension);
    row(Command::ToggleSortDescending);
    row(Command::CycleSortKey);
    row(Command::ToggleRecursive);
    // 並び替えと再帰は既定のキーを持たないので、上の row() はどれも出ない。
    // 唯一の入口であるサイドバーのメニューはここで案内する
    text("一覧のメニュー", "ファイル名一覧で右クリック");

    header("編集");
    row(Command::Undo);
    row(Command::Redo);
    row(Command::ResizeImage);
    row(Command::DeleteAnnotation);
    row(Command::SelectToolCrop);
    row(Command::SelectToolOcr);
    row(Command::SelectToolRect);
    row(Command::SelectToolEllipse);
    row(Command::SelectToolArrow);
    row(Command::SelectToolLine);
    row(Command::SelectToolPen);
    row(Command::SelectToolMarker);
    row(Command::SelectToolNumber);
    row(Command::SelectToolText);
    // Ctrl+B は App が横取りするため Command を持たない(キー変更もできない)
    text("選択中のテキストを太字", "Ctrl+B");

    header("画面");
    row(Command::ToggleSidebar);
    row(Command::ToggleStatusBar);
    row(Command::ToggleHelp);

    header("終了");
    row(Command::Escape);
    row(Command::Quit);

    // マウスの割り当て(既定では前後の画像。[mouse] で変更できる)は Mousemap から
    // 生成する。以降の行は Command を経由しない操作なので固定文言。編集ドラッグの
    // 中身は現在のツール(ステータスバーに出る)で決まる。パンと編集のボタンは
    // [mouse] swap_buttons で入れ替わるので、実際に効くほうを出す
    // (メニューは常に右クリック、オブジェクトを掴む操作は常に左で、どちらも
    //  入れ替えの対象外なので固定文言のまま)
    const std::string_view panDrag = swapMouseButtons ? "右ドラッグ" : "左ドラッグ";
    const std::string_view editDrag = swapMouseButtons ? "左ドラッグ" : "右ドラッグ";
    const std::string_view editDragShift = swapMouseButtons ? "Shift+左ドラッグ" : "Shift+右ドラッグ";
    header("マウス");
    for (const auto& e : kCommandLabels) {
        text(e.label, mouseLabel(mousemap, e.cmd));
    }
    text("拡大 / 縮小", zoomWheelLabel(mousemap));
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
    lines.push_back({"blinker.ini でキー・マウスを", false});
    lines.push_back({"変更できます", false});
    lines.push_back({"(上のキー表記をそのまま書けます)", false});

    return lines;
}

} // namespace blinker
