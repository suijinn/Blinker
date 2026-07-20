#include "win/annotation_draw.h"

#include <algorithm>
#include <cmath>

#include "core/unicode.h"

namespace blinker {

using Microsoft::WRL::ComPtr;

float arrowHeadLength(float strokeWidth) {
    return strokeWidth * 4.0f;
}

namespace {

/// 部分書式の UTF-8 バイト範囲を、レイアウトが使う UTF-16 の範囲へ変換する
DWRITE_TEXT_RANGE toTextRange(const std::string& text, const TextStyleRun& run) {
    const UINT32 begin = static_cast<UINT32>(utf8ToUtf16Offset(text, run.begin));
    const UINT32 end = static_cast<UINT32>(utf8ToUtf16Offset(text, run.end));
    return {begin, end > begin ? end - begin : 0};
}

} // namespace

ComPtr<IDWriteTextLayout> createAnnotationTextLayout(IDWriteFactory* dwrite,
                                                     const AnnotationSpec& spec,
                                                     float maxHeight) {
    ComPtr<IDWriteTextFormat> format;
    if (FAILED(dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                        spec.fontSize, L"ja-jp", &format))) {
        return nullptr;
    }
    const float wrapWidth = std::max(std::abs(spec.p2.x - spec.p1.x), spec.fontSize);
    const std::wstring text = utf8ToWide(spec.text);
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwrite->CreateTextLayout(text.c_str(),
                                        static_cast<UINT32>(text.size()), format.Get(),
                                        wrapWidth, maxHeight, &layout))) {
        return nullptr;
    }
    // 太字・斜体・下線は文字送りや行の高さに影響するため、計測に使うレイアウトにも
    // 反映する(色は見た目だけなので、描画時に drawing effect として与える)
    for (const TextStyleRun& run : spec.styles) {
        const DWRITE_TEXT_RANGE range = toTextRange(spec.text, run);
        if (range.length == 0) continue;
        if (run.bold) layout->SetFontWeight(DWRITE_FONT_WEIGHT_BOLD, range);
        if (run.italic) layout->SetFontStyle(DWRITE_FONT_STYLE_ITALIC, range);
        if (run.underline) layout->SetUnderline(TRUE, range);
    }
    return layout;
}

void applyTextColorEffects(ID2D1RenderTarget* target, IDWriteTextLayout* layout,
                           const AnnotationSpec& spec) {
    for (const TextStyleRun& run : spec.styles) {
        if (!run.hasColor) continue;
        const DWRITE_TEXT_RANGE range = toTextRange(spec.text, run);
        if (range.length == 0) continue;
        // D2D の既定テキストレンダラは drawing effect のブラシで描いてくれる
        ComPtr<ID2D1SolidColorBrush> brush;
        const D2D1_COLOR_F color =
            D2D1::ColorF(((run.colorRGB >> 16) & 0xFF) / 255.0f,
                         ((run.colorRGB >> 8) & 0xFF) / 255.0f, (run.colorRGB & 0xFF) / 255.0f);
        if (FAILED(target->CreateSolidColorBrush(color, &brush))) continue;
        layout->SetDrawingEffect(brush.Get(), range);
    }
}

void drawAnnotationShape(ID2D1RenderTarget* target, ID2D1Factory* factory,
                         IDWriteFactory* dwrite, const AnnotationSpec& spec,
                         ID2D1SolidColorBrush* brush) {
    const D2D1_POINT_2F p1{spec.p1.x, spec.p1.y};
    const D2D1_POINT_2F p2{spec.p2.x, spec.p2.y};
    const float left = std::min(p1.x, p2.x);
    const float top = std::min(p1.y, p2.y);
    const float right = std::max(p1.x, p2.x);
    const float bottom = std::max(p1.y, p2.y);

    switch (spec.kind) {
    case AnnotationSpec::Kind::Rect:
        target->DrawRectangle(D2D1::RectF(left, top, right, bottom), brush, spec.strokeWidth);
        break;
    case AnnotationSpec::Kind::Ellipse:
        target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F((left + right) / 2, (top + bottom) / 2),
                                          (right - left) / 2, (bottom - top) / 2),
                            brush, spec.strokeWidth);
        break;
    case AnnotationSpec::Kind::Line:
        target->DrawLine(p1, p2, brush, spec.strokeWidth);
        break;
    case AnnotationSpec::Kind::Arrow: {
        const float dx = p2.x - p1.x;
        const float dy = p2.y - p1.y;
        const float length = std::sqrt(dx * dx + dy * dy);
        if (length < 0.5f) {
            target->DrawLine(p1, p2, brush, spec.strokeWidth);
            break;
        }
        const float ux = dx / length;
        const float uy = dy / length;
        const float head = std::min(arrowHeadLength(spec.strokeWidth), length);
        // ヘッドの根元まで線を引き、先端は塗りつぶし三角形にする
        const D2D1_POINT_2F base{p2.x - ux * head, p2.y - uy * head};
        target->DrawLine(p1, base, brush, spec.strokeWidth);
        ComPtr<ID2D1PathGeometry> geometry;
        ComPtr<ID2D1GeometrySink> sink;
        if (SUCCEEDED(factory->CreatePathGeometry(&geometry)) &&
            SUCCEEDED(geometry->Open(&sink))) {
            const float halfWidth = head * 0.5f;
            sink->BeginFigure(p2, D2D1_FIGURE_BEGIN_FILLED);
            sink->AddLine(D2D1::Point2F(base.x - uy * halfWidth, base.y + ux * halfWidth));
            sink->AddLine(D2D1::Point2F(base.x + uy * halfWidth, base.y - ux * halfWidth));
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->Close();
            target->FillGeometry(geometry.Get(), brush);
        }
        break;
    }
    case AnnotationSpec::Kind::Text: {
        // レイアウトの高さ制限は不要なため十分大きい値を渡す
        const ComPtr<IDWriteTextLayout> layout =
            createAnnotationTextLayout(dwrite, spec, 16384.0f);
        if (!layout) break;
        // 部分書式の色。指定の無い範囲は brush(注釈全体の色)のまま描かれる
        applyTextColorEffects(target, layout.Get(), spec);
        target->DrawTextLayout(D2D1::Point2F(left, top), layout.Get(), brush);
        break;
    }
    }
}

} // namespace blinker
