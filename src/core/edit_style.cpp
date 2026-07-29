#include "core/edit_style.h"

#include <algorithm>
#include <optional>
#include <string_view>

#include "core/str_util.h"

namespace blinker {
namespace {

// blinker.ini の [edit] tool = に書く名前。[keys] の tool_* コマンド名と揃える
std::optional<EditTool> toolFromName(std::string_view name) {
    const std::string lower = toLower(trim(name));
    if (lower == "crop") return EditTool::Crop;
    if (lower == "rect") return EditTool::Rect;
    if (lower == "ellipse") return EditTool::Ellipse;
    if (lower == "arrow") return EditTool::Arrow;
    if (lower == "line") return EditTool::Line;
    if (lower == "pen") return EditTool::Pen;
    if (lower == "marker") return EditTool::Marker;
    if (lower == "number") return EditTool::Number;
    if (lower == "text") return EditTool::Text;
    if (lower == "ocr") return EditTool::Ocr;
    return std::nullopt;
}

}  // namespace

std::string_view toolLabel(const EditTool tool) {
    switch (tool) {
    case EditTool::Crop:    return "トリミング";
    case EditTool::Rect:    return "矩形";
    case EditTool::Ellipse: return "楕円";
    case EditTool::Arrow:   return "矢印";
    case EditTool::Line:    return "直線";
    case EditTool::Pen:     return "ペン (手書き)";
    case EditTool::Marker:  return "マーカー";
    case EditTool::Number:  return "連番マーカー";
    case EditTool::Text:    return "テキスト";
    case EditTool::Ocr:     return "文字認識";
    }
    return "";
}

void EditStyle::setTool(EditTool tool) {
    // トリミングは一度きりなので、戻り先として直前の図形ツールを覚えておく
    if (tool != EditTool::Crop) toolAfterCrop_ = tool;
    tool_ = tool;
}

bool EditStyle::penActive() const {
    return tool_ == EditTool::Pen || tool_ == EditTool::Marker;
}

void EditStyle::setStrokeWidth(float px) {
    strokeWidth_ = std::clamp(px, 1.0f, 100.0f);
}

void EditStyle::setFontSize(float px) {
    fontSize_ = std::clamp(px, 6.0f, 200.0f);
}

void EditStyle::setFontFamily(std::string family) {
    if (family.empty()) return;
    fontFamily_ = std::move(family);
}

void EditStyle::setFillColor(uint32_t rgb) {
    fillRGB_ = rgb;
    // 色を選んだのに塗られないままにならないよう、塗りなしなら不透明で始める
    if (fillAlpha_ == 0) fillAlpha_ = 255;
}

void EditStyle::setFillAlpha(int alpha) {
    fillAlpha_ = std::clamp(alpha, 0, 255);
}

void EditStyle::setBorderColor(uint32_t rgb) {
    borderRGB_ = rgb;
    if (borderWidth_ <= 0) borderWidth_ = 1.0f;
}

void EditStyle::setBorderWidth(float px) {
    borderWidth_ = std::clamp(px, 0.0f, 100.0f);
}

void EditStyle::applyTo(AnnotationSpec& spec) const {
    spec.colorRGB = colorRGB_;
    spec.fillRGB = fillRGB_;
    spec.fillAlpha = fillAlpha_;
    spec.borderRGB = borderRGB_;
    spec.fontFamily = fontFamily_;
    spec.strokeWidth = strokeWidth_;
    spec.fontSize = fontSize_;
    spec.borderWidth = borderWidth_;
    if (spec.kind == AnnotationSpec::Kind::Pen && tool_ == EditTool::Marker) {
        spec.strokeWidth = strokeWidth_ * kMarkerWidthScale;
        spec.strokeAlpha = kMarkerAlpha;
    }
}

void EditStyle::applyConfig(const Config& config) {
    colorRGB_ = config.getColorRGB("edit", "color", colorRGB_);
    setStrokeWidth(
        static_cast<float>(config.getInt("edit", "stroke_width", static_cast<int>(strokeWidth_))));
    setFontSize(
        static_cast<float>(config.getInt("edit", "font_size", static_cast<int>(fontSize_))));
    // フォントは候補表に無い名前も受け付ける(入っていなければ描画側がフォールバックする)。
    // 存在確認はしない(起動時にシステムフォントを列挙したくないため)
    setFontFamily(std::string(trim(config.get("edit", "font_family"))));
    // 色だけを指定して不透明度を書かなかった場合に、勝手に塗り・枠線が出ないよう
    // セッターは通さない(setFillColor / setBorderColor の補正は手で選んだときだけ)
    fillRGB_ = config.getColorRGB("edit", "fill_color", fillRGB_);
    setFillAlpha(config.getInt("edit", "fill_alpha", fillAlpha_));
    borderRGB_ = config.getColorRGB("edit", "border_color", borderRGB_);
    setBorderWidth(
        static_cast<float>(config.getInt("edit", "border_width", static_cast<int>(borderWidth_))));
    // 起動時の(編集ドラッグで実行される)ツール。未指定・未知の名前なら既定のまま
    if (const auto tool = toolFromName(config.get("edit", "tool"))) setTool(*tool);
}

}  // namespace blinker
