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

/// 描画に使うフォントファミリ名。未指定なら既定
std::wstring fontFamilyOf(const AnnotationSpec& spec) {
    return utf8ToWide(spec.fontFamily.empty() ? kDefaultFontFamily : spec.fontFamily);
}

D2D1_COLOR_F colorFrom(uint32_t rgb, float alpha) {
    return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f,
                        (rgb & 0xFF) / 255.0f, alpha);
}

/// 塗りつぶし用のブラシを作る。塗らない指定(fillAlpha = 0)なら nullptr
ComPtr<ID2D1SolidColorBrush> createFillBrush(ID2D1RenderTarget* target,
                                             const AnnotationSpec& spec) {
    if (spec.fillAlpha <= 0) return nullptr;
    ComPtr<ID2D1SolidColorBrush> brush;
    const float alpha = std::clamp(spec.fillAlpha, 0, 255) / 255.0f;
    if (FAILED(target->CreateSolidColorBrush(colorFrom(spec.fillRGB, alpha), &brush))) {
        return nullptr;
    }
    return brush;
}

} // namespace

ComPtr<IDWriteTextLayout> createAnnotationTextLayout(IDWriteFactory* dwrite,
                                                     const AnnotationSpec& spec,
                                                     float maxHeight) {
    ComPtr<IDWriteTextFormat> format;
    // 入っていないファミリ名でも CreateTextFormat は成功し、DirectWrite の
    // フォールバックで描かれる(文字が出なくなることはない)
    if (FAILED(dwrite->CreateTextFormat(fontFamilyOf(spec).c_str(), nullptr,
                                        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                        DWRITE_FONT_STRETCH_NORMAL, spec.fontSize, L"ja-jp",
                                        &format))) {
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
    // 太字・斜体・下線・フォントは文字送りや行の高さに影響するため、計測に使う
    // レイアウトにも反映する(色は見た目だけなので、描画時に drawing effect として与える)
    for (const TextStyleRun& run : spec.styles) {
        const DWRITE_TEXT_RANGE range = toTextRange(spec.text, run);
        if (range.length == 0) continue;
        if (run.bold) layout->SetFontWeight(DWRITE_FONT_WEIGHT_BOLD, range);
        if (run.italic) layout->SetFontStyle(DWRITE_FONT_STYLE_ITALIC, range);
        if (run.underline) layout->SetUnderline(TRUE, range);
        if (!run.fontFamily.empty()) {
            layout->SetFontFamilyName(utf8ToWide(run.fontFamily).c_str(), range);
        }
    }
    return layout;
}

D2D1_RECT_F textBoxRect(const AnnotationSpec& spec, IDWriteTextLayout* layout) {
    const float left = std::min(spec.p1.x, spec.p2.x);
    const float top = std::min(spec.p1.y, spec.p2.y);
    DWRITE_TEXT_METRICS metrics{};
    if (!layout || FAILED(layout->GetMetrics(&metrics))) {
        return D2D1::RectF(left, top, std::max(spec.p1.x, spec.p2.x),
                           std::max(spec.p1.y, spec.p2.y));
    }
    return D2D1::RectF(left, top,
                       left + std::max(metrics.widthIncludingTrailingWhitespace, 1.0f),
                       top + std::max(metrics.height, 1.0f));
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
                         ID2D1SolidColorBrush* brush, AnnotationDrawParts parts) {
    const D2D1_POINT_2F p1{spec.p1.x, spec.p1.y};
    const D2D1_POINT_2F p2{spec.p2.x, spec.p2.y};
    const float left = std::min(p1.x, p2.x);
    const float top = std::min(p1.y, p2.y);
    const float right = std::max(p1.x, p2.x);
    const float bottom = std::max(p1.y, p2.y);
    const bool background = parts != AnnotationDrawParts::Foreground;
    const bool foreground = parts != AnnotationDrawParts::Background;

    // 塗りつぶしは輪郭線の下に敷く(線の内側半分が塗りの縁を隠す)
    const ComPtr<ID2D1SolidColorBrush> fill =
        background ? createFillBrush(target, spec) : nullptr;

    switch (spec.kind) {
    case AnnotationSpec::Kind::Rect:
        if (fill) target->FillRectangle(D2D1::RectF(left, top, right, bottom), fill.Get());
        if (foreground) {
            target->DrawRectangle(D2D1::RectF(left, top, right, bottom), brush,
                                  spec.strokeWidth);
        }
        break;
    case AnnotationSpec::Kind::Ellipse: {
        const D2D1_ELLIPSE ellipse =
            D2D1::Ellipse(D2D1::Point2F((left + right) / 2, (top + bottom) / 2),
                          (right - left) / 2, (bottom - top) / 2);
        if (fill) target->FillEllipse(ellipse, fill.Get());
        if (foreground) target->DrawEllipse(ellipse, brush, spec.strokeWidth);
        break;
    }
    case AnnotationSpec::Kind::Line:
        // 直線・矢印は面を持たないため背景は無い
        if (foreground) target->DrawLine(p1, p2, brush, spec.strokeWidth);
        break;
    case AnnotationSpec::Kind::Arrow: {
        if (!foreground) break;
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
        if (background) {
            // 塗りつぶし・枠線は実測の箱(ラスタライザが確保する領域と同じ)へ描く
            const D2D1_RECT_F box = textBoxRect(spec, layout.Get());
            if (fill) target->FillRectangle(box, fill.Get());
            if (spec.borderWidth > 0) {
                ComPtr<ID2D1SolidColorBrush> border;
                if (SUCCEEDED(target->CreateSolidColorBrush(colorFrom(spec.borderRGB, 1.0f),
                                                            &border))) {
                    target->DrawRectangle(box, border.Get(), spec.borderWidth);
                }
            }
        }
        if (foreground) {
            // 部分書式の色。指定の無い範囲は brush(注釈全体の色)のまま描かれる
            applyTextColorEffects(target, layout.Get(), spec);
            target->DrawTextLayout(D2D1::Point2F(left, top), layout.Get(), brush);
        }
        break;
    }
    }
}

} // namespace blinker
