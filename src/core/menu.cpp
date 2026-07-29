#include "core/menu.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <utility>

#include "core/command.h"
#include "core/help.h"
#include "core/image_scale.h"

namespace blinker {

namespace {

// 塗りつぶしの不透明度の選択肢(0-255)。0 は塗らない = 完全な透過
constexpr std::array<int, 5> kFillAlphaChoices{0, 64, 128, 191, 255};
// テキストの枠線幅の選択肢(画面px基準)。0 は枠線なし
constexpr std::array<int, 6> kBorderWidthChoices{0, 1, 2, 3, 5, 8};

// フォントの選択肢。{表示ラベル, 描画側へ渡すファミリ名}。和文ゴシック・明朝・
// UD・欧文・等幅を一通り並べてある。入っていないものはメニューに出さない
constexpr std::array<std::pair<const char*, const char*>, 9> kFontFamilyChoices{{
    {"游ゴシック", "Yu Gothic"},
    {"游ゴシック UI", "Yu Gothic UI"},
    {"游明朝", "Yu Mincho"},
    {"メイリオ", "Meiryo"},
    {"BIZ UDPゴシック", "BIZ UDPGothic"},
    {"MS ゴシック", "MS Gothic"},
    {"MS 明朝", "MS Mincho"},
    {"Segoe UI", "Segoe UI"},
    {"Consolas", "Consolas"},
}};

std::string fillAlphaLabel(int alpha) {
    if (alpha <= 0) return "なし (透明)";
    return std::format("{}%", std::lround(alpha * 100.0 / 255.0));
}

std::string borderWidthLabel(int width) {
    return width <= 0 ? std::string("なし") : std::format("{}px", width);
}

// メニューの見出しに出す名前。候補表にあれば日本語ラベル、無ければファミリ名のまま
std::string fontFamilyLabel(std::string_view family) {
    const std::string_view name = effectiveFontFamily(family);
    for (const auto& [label, value] : kFontFamilyChoices) {
        if (name == value) return label;
    }
    return std::string(name);
}

// ツール切り替えに対応するコマンド。メニュー項目にキー表記を出すために使う
Command commandOfTool(EditTool tool) {
    switch (tool) {
    case EditTool::Crop:    return Command::SelectToolCrop;
    case EditTool::Rect:    return Command::SelectToolRect;
    case EditTool::Ellipse: return Command::SelectToolEllipse;
    case EditTool::Arrow:   return Command::SelectToolArrow;
    case EditTool::Line:    return Command::SelectToolLine;
    case EditTool::Pen:     return Command::SelectToolPen;
    case EditTool::Marker:  return Command::SelectToolMarker;
    case EditTool::Number:  return Command::SelectToolNumber;
    case EditTool::Text:    return Command::SelectToolText;
    case EditTool::Ocr:     return Command::SelectToolOcr;
    }
    return Command::None;
}

// 並び替えキーに対応するコマンド。メニュー項目にキー表記を出すために使う
Command commandOfSortKey(SortKey key) {
    switch (key) {
    case SortKey::Name:      return Command::SortByName;
    case SortKey::Date:      return Command::SortByDate;
    case SortKey::Size:      return Command::SortBySize;
    case SortKey::Extension: return Command::SortByExtension;
    }
    return Command::None;
}

// キーを割り当ててあれば、'\t' 区切りで右寄せのアクセラレータ表記を足す
std::string withKeys(std::string text, const Keymap& keymap, const Command cmd) {
    if (const std::string keys = keysLabel(keymap, cmd); !keys.empty()) {
        text += '\t';
        text += keys;
    }
    return text;
}

// 末端項目を entries へ積みつつ、対応するメニュー項目を返す。
// entries の並びが showContextMenu の返す index に対応する
template <typename Entry>
MenuItem leafItem(std::vector<Entry>& entries, std::string text, Entry entry, const bool checked) {
    entries.push_back(std::move(entry));
    MenuItem item;
    item.text = std::move(text);
    item.checked = checked;
    return item;
}

}  // namespace

// 区切り線のメニュー項目({.separator = true} は gcc の
// -Wmissing-field-initializers 警告になるため関数にする)
MenuItem menuSeparator() {
    MenuItem item;
    item.separator = true;
    return item;
}

std::string_view effectiveFontFamily(std::string_view family) {
    return family.empty() ? std::string_view(kDefaultFontFamily) : family;
}

FontFamilyChoices fontFamilyChoices(std::string_view current, const FontAvailableFn& available) {
    const std::string_view effective = effectiveFontFamily(current);
    FontFamilyChoices choices;
    bool listed = false;
    for (const auto& [label, family] : kFontFamilyChoices) {
        if (effective == family) {
            listed = true;  // 現在のフォントは、入っていなくても選び直せるよう必ず出す
        } else if (!available(family)) {
            continue;
        }
        choices.push_back({label, family});
    }
    if (!listed) choices.push_back({std::string(effective), std::string(effective)});
    return choices;
}

std::vector<MenuItem> buildEditMenu(const EditStyle& style, const Keymap& keymap,
                                    const FontAvailableFn& available,
                                    const std::optional<MenuImageSize> image,
                                    std::vector<EditMenuEntry>& entries) {
    const auto leaf = [&entries](std::string text, EditMenuEntry entry, bool checked = false) {
        return leafItem(entries, std::move(text), std::move(entry), checked);
    };
    // ツールは選ぶだけで、実際の適用は次の編集ドラッグで行う。現在のツールにチェックが付く。
    // ini でキーを割り当てていれば、'\t' 区切りで右寄せのアクセラレータ表記として出る
    const auto tool = [&leaf, &keymap, &style](EditTool t) {
        return leaf(withKeys(std::string(toolLabel(t)), keymap, commandOfTool(t)),
                    {EditMenuEntry::Action::SelectTool, t, 0}, t == style.tool());
    };
    using Action = EditMenuEntry::Action;

    std::vector<MenuItem> items;
    items.push_back(tool(EditTool::Crop));
    items.push_back(tool(EditTool::Ocr));
    items.push_back(menuSeparator());
    items.push_back(tool(EditTool::Rect));
    items.push_back(tool(EditTool::Ellipse));
    items.push_back(tool(EditTool::Arrow));
    items.push_back(tool(EditTool::Line));
    items.push_back(tool(EditTool::Pen));
    items.push_back(tool(EditTool::Marker));
    items.push_back(tool(EditTool::Number));
    items.push_back(tool(EditTool::Text));
    items.push_back(menuSeparator());

    MenuItem stroke;
    stroke.text = std::format("線の太さ ({}px)", static_cast<int>(style.strokeWidth()));
    for (const int w : {1, 2, 3, 5, 8, 12, 20}) {
        stroke.children.push_back(
            leaf(std::format("{}px", w),
                 {Action::StrokeWidth, EditTool::Rect, static_cast<float>(w)},
                 static_cast<float>(w) == style.strokeWidth()));
    }
    items.push_back(std::move(stroke));

    MenuItem font;
    font.text = std::format("文字サイズ ({}px)", static_cast<int>(style.fontSize()));
    for (const int s : {12, 14, 18, 24, 36, 48, 72}) {
        font.children.push_back(
            leaf(std::format("{}px", s),
                 {Action::FontSize, EditTool::Rect, static_cast<float>(s)},
                 static_cast<float>(s) == style.fontSize()));
    }
    items.push_back(std::move(font));

    MenuItem family;
    family.text = std::format("フォント ({})", fontFamilyLabel(style.fontFamily()));
    for (const auto& [label, name] : fontFamilyChoices(style.fontFamily(), available)) {
        family.children.push_back(leaf(label, {Action::FontFamily, EditTool::Rect, 0, name},
                                       name == style.fontFamily()));
    }
    items.push_back(std::move(family));

    items.push_back(
        leaf(std::format("色の変更... (#{:06X})", style.colorRGB()), {Action::PickColor}));

    // 塗りつぶし(矩形・楕円・テキスト)。不透明度 0 で塗らない = 背景が透ける
    MenuItem fill;
    fill.text = std::format("塗りつぶし ({})", fillAlphaLabel(style.fillAlpha()));
    for (const int a : kFillAlphaChoices) {
        fill.children.push_back(
            leaf(fillAlphaLabel(a), {Action::FillAlpha, EditTool::Rect, static_cast<float>(a)},
                 a == style.fillAlpha()));
    }
    fill.children.push_back(menuSeparator());
    fill.children.push_back(leaf(std::format("色の変更... (#{:06X})", style.fillRGB()),
                                 {Action::PickFillColor}));
    items.push_back(std::move(fill));

    // 枠線はテキストボックス用(矩形・楕円の輪郭は「線の太さ」「色の変更」で指定する)
    MenuItem border;
    border.text = std::format("テキストの枠線 ({})",
                              borderWidthLabel(static_cast<int>(style.borderWidth())));
    for (const int w : kBorderWidthChoices) {
        border.children.push_back(
            leaf(borderWidthLabel(w),
                 {Action::BorderWidth, EditTool::Rect, static_cast<float>(w)},
                 static_cast<float>(w) == style.borderWidth()));
    }
    border.children.push_back(menuSeparator());
    border.children.push_back(leaf(std::format("色の変更... (#{:06X})", style.borderRGB()),
                                   {Action::PickBorderColor}));
    items.push_back(std::move(border));

    // リサイズはツールではなく一度きりの操作だが、画像に対する操作の入口がこのメニュー
    // しかないのでここへ置く(Command::ResizeImage は同じ内容を単独で出す)
    if (image) {
        items.push_back(menuSeparator());
        MenuItem resize;
        resize.text = std::format("画像をリサイズ ({} x {})", image->width, image->height);
        resize.children = buildResizeMenu(*image, entries);
        items.push_back(std::move(resize));
    }
    return items;
}

std::vector<MenuItem> buildResizeMenu(const MenuImageSize image,
                                      std::vector<EditMenuEntry>& entries) {
    const auto leaf = [&entries](std::string text, EditMenuEntry entry) {
        return leafItem(entries, std::move(text), std::move(entry), false);
    };
    using Action = EditMenuEntry::Action;
    // 変換後の大きさを項目に添える(選ぶ前に結果が分かるように)
    const auto label = [image](const std::string& head, const double factor) {
        const auto [w, h] = scaledSize(image.width, image.height, factor);
        return std::format("{}\t{} x {}", head, w, h);
    };

    std::vector<MenuItem> items;
    MenuItem percent;
    percent.text = "倍率";
    for (const int p : {200, 150, 75, 50, 25}) {
        percent.children.push_back(
            leaf(label(std::format("{}%", p), p / 100.0),
                 {Action::ResizePercent, EditTool::Rect, static_cast<float>(p)}));
    }
    items.push_back(std::move(percent));

    MenuItem longEdge;
    longEdge.text = "長辺を指定";
    const uint32_t longest = std::max(image.width, image.height);
    for (const int e : {3840, 2560, 1920, 1280, 1024, 800, 640}) {
        const double factor = longest > 0 ? static_cast<double>(e) / longest : 1.0;
        longEdge.children.push_back(
            leaf(label(std::format("{} px", e), factor),
                 {Action::ResizeLongEdge, EditTool::Rect, static_cast<float>(e)}));
    }
    items.push_back(std::move(longEdge));
    return items;
}

std::vector<MenuItem> buildSidebarMenu(const SortOrder order, const bool recursive,
                                       const Keymap& keymap,
                                       std::vector<SidebarMenuEntry>& entries) {
    const auto leaf = [&entries](std::string text, SidebarMenuEntry entry,
                                 const bool checked = false) {
        return leafItem(entries, std::move(text), std::move(entry), checked);
    };
    using Action = SidebarMenuEntry::Action;
    const auto sortKey = [&leaf, &keymap, order](const std::string_view label, const SortKey key) {
        return leaf(withKeys(std::string(label), keymap, commandOfSortKey(key)),
                    {Action::SortKey, key}, key == order.key);
    };

    std::vector<MenuItem> items;
    MenuItem sort;
    sort.text = std::format("並び替え ({})", sortOrderLabel(order));
    sort.children.push_back(sortKey("名前", SortKey::Name));
    sort.children.push_back(sortKey("更新日時", SortKey::Date));
    sort.children.push_back(sortKey("サイズ", SortKey::Size));
    sort.children.push_back(sortKey("種類", SortKey::Extension));
    items.push_back(std::move(sort));
    items.push_back(menuSeparator());
    items.push_back(leaf("昇順", {Action::SortAscending}, !order.descending));
    items.push_back(leaf("降順", {Action::SortDescending}, order.descending));
    items.push_back(menuSeparator());

    items.push_back(leaf(withKeys("サブフォルダを含める", keymap, Command::ToggleRecursive),
                         {Action::ToggleRecursive}, recursive));
    return items;
}

std::vector<MenuItem> buildObjectMenu(const AnnotationSpec& spec, const Keymap& keymap,
                                      const FontAvailableFn& available,
                                      std::vector<ObjectMenuEntry>& entries) {
    const auto leaf = [&entries](std::string text, ObjectMenuEntry entry, bool checked = false) {
        return leafItem(entries, std::move(text), std::move(entry), checked);
    };
    using Action = ObjectMenuEntry::Action;

    std::vector<MenuItem> items;
    if (spec.kind == AnnotationSpec::Kind::Text) {
        items.push_back(leaf("テキストを編集", {Action::EditText}));
    }
    items.push_back(leaf(withKeys("削除", keymap, Command::DeleteAnnotation), {Action::Delete}));
    items.push_back(menuSeparator());

    MenuItem angle;
    angle.text = std::format("回転角度 ({}°)", static_cast<int>(std::lround(spec.angleDeg)));
    for (const int a : {0, 15, 30, 45, 90, 135, 180, 270}) {
        angle.children.push_back(leaf(std::format("{}°", a),
                                      {Action::Angle, static_cast<float>(a)},
                                      static_cast<float>(a) == spec.angleDeg));
    }
    items.push_back(std::move(angle));

    // 太さ・文字サイズは画像px単位でオブジェクトへ直接適用する
    if (spec.kind == AnnotationSpec::Kind::Text) {
        MenuItem font;
        font.text =
            std::format("文字サイズ ({}px)", static_cast<int>(std::lround(spec.fontSize)));
        for (const int s : {12, 14, 18, 24, 36, 48, 72}) {
            font.children.push_back(leaf(std::format("{}px", s),
                                         {Action::FontSize, static_cast<float>(s)},
                                         static_cast<float>(s) == spec.fontSize));
        }
        items.push_back(std::move(font));

        MenuItem family;
        family.text = std::format("フォント ({})", fontFamilyLabel(spec.fontFamily));
        // 未指定(空)の注釈は既定フォントで描かれるので、そちらにチェックを付ける
        const std::string_view current = effectiveFontFamily(spec.fontFamily);
        for (const auto& [label, name] : fontFamilyChoices(current, available)) {
            family.children.push_back(
                leaf(label, {Action::FontFamily, 0, name}, name == current));
        }
        items.push_back(std::move(family));
    } else if (spec.kind != AnnotationSpec::Kind::Image) {
        // 貼り付けた画像は線も文字も持たないので、太さの項目は出さない
        MenuItem stroke;
        stroke.text =
            std::format("線の太さ ({}px)", static_cast<int>(std::lround(spec.strokeWidth)));
        // 手書き(特にマーカー)は太い側も要るので、選択肢を広げる
        const std::vector<int> widths = spec.kind == AnnotationSpec::Kind::Pen
                                            ? std::vector<int>{1, 2, 3, 5, 8, 12, 20, 32, 48}
                                            : std::vector<int>{1, 2, 3, 5, 8, 12, 20};
        for (const int w : widths) {
            stroke.children.push_back(leaf(std::format("{}px", w),
                                           {Action::StrokeWidth, static_cast<float>(w)},
                                           static_cast<float>(w) == spec.strokeWidth));
        }
        items.push_back(std::move(stroke));
    }
    // 線の不透明度は手書きだけ(マーカーとペンの違いはここと太さだけ)
    if (spec.kind == AnnotationSpec::Kind::Pen) {
        MenuItem alpha;
        alpha.text = std::format("線の不透明度 ({})", fillAlphaLabel(spec.strokeAlpha));
        for (const int a : {255, 178, 102, 64}) {
            alpha.children.push_back(leaf(fillAlphaLabel(a),
                                          {Action::StrokeAlpha, static_cast<float>(a)},
                                          a == spec.strokeAlpha));
        }
        items.push_back(std::move(alpha));
    }
    // 連番は後から振り直せるようにする(順序を入れ替えたくなることがある)
    if (spec.kind == AnnotationSpec::Kind::Number) {
        MenuItem number;
        number.text = std::format("番号 ({})", spec.number);
        for (int n = 1; n <= 10; ++n) {
            number.children.push_back(leaf(std::format("{}", n),
                                           {Action::Number, static_cast<float>(n)},
                                           n == spec.number));
        }
        items.push_back(std::move(number));
    }
    // 画像は自身の画素で描かれるため、色も塗りつぶしも効かない
    if (spec.kind != AnnotationSpec::Kind::Image) {
        items.push_back(
            leaf(std::format("色の変更... (#{:06X})", spec.colorRGB), {Action::PickColor}));
    }

    // 塗りつぶしは面を持つ種別だけ(直線・矢印・手書き・画像には出さない)
    if (spec.kind != AnnotationSpec::Kind::Line && spec.kind != AnnotationSpec::Kind::Arrow &&
        spec.kind != AnnotationSpec::Kind::Pen && spec.kind != AnnotationSpec::Kind::Image) {
        MenuItem fill;
        fill.text = std::format("塗りつぶし ({})", fillAlphaLabel(spec.fillAlpha));
        for (const int a : kFillAlphaChoices) {
            fill.children.push_back(leaf(fillAlphaLabel(a),
                                         {Action::FillAlpha, static_cast<float>(a)},
                                         a == spec.fillAlpha));
        }
        fill.children.push_back(menuSeparator());
        fill.children.push_back(leaf(std::format("色の変更... (#{:06X})", spec.fillRGB),
                                     {Action::PickFillColor}));
        items.push_back(std::move(fill));
    }
    if (spec.kind == AnnotationSpec::Kind::Text) {
        const int borderWidth = static_cast<int>(std::lround(spec.borderWidth));
        MenuItem border;
        border.text = std::format("枠線 ({})", borderWidthLabel(borderWidth));
        for (const int w : kBorderWidthChoices) {
            border.children.push_back(leaf(borderWidthLabel(w),
                                           {Action::BorderWidth, static_cast<float>(w)},
                                           static_cast<float>(w) == spec.borderWidth));
        }
        border.children.push_back(menuSeparator());
        border.children.push_back(leaf(std::format("色の変更... (#{:06X})", spec.borderRGB),
                                       {Action::PickBorderColor}));
        items.push_back(std::move(border));
    }
    return items;
}

TextStyleMenu buildTextStyleMenu(const AnnotationSpec& spec, const TextEditBuffer& buffer,
                                 const FontAvailableFn& available) {
    // トグルできる属性を並べ、続いてフォント、最後に文字色。
    // 末端項目の index はこの順に対応する
    TextStyleMenu menu;
    for (const auto& [text, flag] : kTextStyleFlags) {
        MenuItem item;
        item.text = text;
        item.checked = buffer.selectionHasFlag(flag);
        menu.items.push_back(std::move(item));
    }
    menu.items.push_back(menuSeparator());
    // 色・フォントを指定していない範囲は注釈全体のもので描かれるので、
    // そちらを見出しと初期値に使う
    const TextStyleRun style = buffer.selectionStyle();
    menu.initialColor = style.hasColor ? style.colorRGB : spec.colorRGB;
    menu.wholeFamily = std::string(effectiveFontFamily(spec.fontFamily));
    const std::string_view currentFamily = style.fontFamily.empty()
                                               ? std::string_view(menu.wholeFamily)
                                               : std::string_view(style.fontFamily);

    // 末端 index: 0-2 が太字・斜体・下線、続いてフォントの候補、最後に文字色
    menu.families = fontFamilyChoices(currentFamily, available);
    MenuItem family;
    family.text = std::format("フォント ({})", fontFamilyLabel(currentFamily));
    for (const auto& [label, name] : menu.families) {
        MenuItem item;
        item.text = label;
        item.checked = name == currentFamily;
        family.children.push_back(std::move(item));
    }
    menu.items.push_back(std::move(family));

    MenuItem color;
    color.text = std::format("文字色... (#{:06X})", menu.initialColor);
    menu.items.push_back(std::move(color));

    menu.familyBase = kTextStyleFlags.size();                 // フォント候補の先頭
    menu.colorIndex = menu.familyBase + menu.families.size();  // 文字色
    return menu;
}

}  // namespace blinker
