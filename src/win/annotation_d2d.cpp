#include "win/annotation_d2d.h"

#include <wincodec.h>

#include <algorithm>
#include <cmath>

#include "win/wic_factory.h"

namespace blinker {
namespace {

using Microsoft::WRL::ComPtr;

constexpr float kMaxOverlaySize = 16384.0f;  // DecoderWic の上限と同じ

D2D1_COLOR_F colorFromRGB(uint32_t rgb) {
    return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f,
                        (rgb & 0xFF) / 255.0f);
}

struct BoundsF {
    float minX = 0, minY = 0, maxX = 0, maxY = 0;
};

// 矢印ヘッド(塗りつぶし三角形)の長さ。線幅に比例させる
float arrowHeadLength(float strokeWidth) {
    return strokeWidth * 4.0f;
}

} // namespace

AnnotationD2D::AnnotationD2D() {
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory_.GetAddressOf());
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                        reinterpret_cast<IUnknown**>(dwriteFactory_.GetAddressOf()));
}

AnnotationOverlay AnnotationD2D::rasterize(const AnnotationSpec& spec) {
    IWICImagingFactory* wic = wicFactoryForThisThread();
    if (!factory_ || !dwriteFactory_ || !wic) return {};

    // テキストはレイアウトの実測、図形は端点+線幅からバウンディングボックスを決める
    BoundsF bounds{std::min(spec.p1.x, spec.p2.x), std::min(spec.p1.y, spec.p2.y),
                   std::max(spec.p1.x, spec.p2.x), std::max(spec.p1.y, spec.p2.y)};
    ComPtr<IDWriteTextLayout> textLayout;
    if (spec.kind == AnnotationSpec::Kind::Text) {
        ComPtr<IDWriteTextFormat> format;
        if (FAILED(dwriteFactory_->CreateTextFormat(
                L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, spec.fontSize, L"ja-jp", &format))) {
            return {};
        }
        // 選択領域の幅で折り返す(幅が小さすぎるときは1文字分を確保)
        const float wrapWidth = std::max(bounds.maxX - bounds.minX, spec.fontSize);
        if (FAILED(dwriteFactory_->CreateTextLayout(spec.text.c_str(),
                                                    static_cast<UINT32>(spec.text.size()),
                                                    format.Get(), wrapWidth, kMaxOverlaySize,
                                                    &textLayout))) {
            return {};
        }
        DWRITE_TEXT_METRICS metrics{};
        textLayout->GetMetrics(&metrics);
        bounds.maxX = bounds.minX + std::max(metrics.widthIncludingTrailingWhitespace, 1.0f);
        bounds.maxY = bounds.minY + std::max(metrics.height, 1.0f);
    }
    const float margin = spec.kind == AnnotationSpec::Kind::Text
                             ? 2.0f
                             : spec.strokeWidth + arrowHeadLength(spec.strokeWidth);
    const int originX = static_cast<int>(std::floor(bounds.minX - margin));
    const int originY = static_cast<int>(std::floor(bounds.minY - margin));
    const float widthF = std::ceil(bounds.maxX + margin) - static_cast<float>(originX);
    const float heightF = std::ceil(bounds.maxY + margin) - static_cast<float>(originY);
    if (widthF <= 0 || heightF <= 0 || widthF > kMaxOverlaySize || heightF > kMaxOverlaySize) {
        return {};
    }
    const UINT width = static_cast<UINT>(widthF);
    const UINT height = static_cast<UINT>(heightF);

    ComPtr<IWICBitmap> wicBitmap;
    if (FAILED(wic->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA,
                                 WICBitmapCacheOnDemand, &wicBitmap))) {
        return {};
    }
    ComPtr<ID2D1RenderTarget> target;
    const auto props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f,
        96.0f);
    if (FAILED(factory_->CreateWicBitmapRenderTarget(wicBitmap.Get(), props, &target))) {
        return {};
    }
    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(target->CreateSolidColorBrush(colorFromRGB(spec.colorRGB), &brush))) return {};

    // ローカル座標 = 画像座標 - origin
    const D2D1_POINT_2F p1{spec.p1.x - originX, spec.p1.y - originY};
    const D2D1_POINT_2F p2{spec.p2.x - originX, spec.p2.y - originY};
    const float left = std::min(p1.x, p2.x);
    const float top = std::min(p1.y, p2.y);
    const float right = std::max(p1.x, p2.x);
    const float bottom = std::max(p1.y, p2.y);

    target->BeginDraw();
    target->Clear(D2D1::ColorF(0, 0.0f));
    switch (spec.kind) {
    case AnnotationSpec::Kind::Rect:
        target->DrawRectangle(D2D1::RectF(left, top, right, bottom), brush.Get(),
                              spec.strokeWidth);
        break;
    case AnnotationSpec::Kind::Ellipse:
        target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F((left + right) / 2, (top + bottom) / 2),
                                          (right - left) / 2, (bottom - top) / 2),
                            brush.Get(), spec.strokeWidth);
        break;
    case AnnotationSpec::Kind::Line:
        target->DrawLine(p1, p2, brush.Get(), spec.strokeWidth);
        break;
    case AnnotationSpec::Kind::Arrow: {
        const float dx = p2.x - p1.x;
        const float dy = p2.y - p1.y;
        const float length = std::sqrt(dx * dx + dy * dy);
        if (length < 0.5f) {
            target->DrawLine(p1, p2, brush.Get(), spec.strokeWidth);
            break;
        }
        const float ux = dx / length;
        const float uy = dy / length;
        const float head = std::min(arrowHeadLength(spec.strokeWidth), length);
        // ヘッドの根元まで線を引き、先端は塗りつぶし三角形にする
        const D2D1_POINT_2F base{p2.x - ux * head, p2.y - uy * head};
        target->DrawLine(p1, base, brush.Get(), spec.strokeWidth);
        ComPtr<ID2D1PathGeometry> geometry;
        ComPtr<ID2D1GeometrySink> sink;
        if (SUCCEEDED(factory_->CreatePathGeometry(&geometry)) &&
            SUCCEEDED(geometry->Open(&sink))) {
            const float halfWidth = head * 0.5f;
            sink->BeginFigure(p2, D2D1_FIGURE_BEGIN_FILLED);
            sink->AddLine(D2D1::Point2F(base.x - uy * halfWidth, base.y + ux * halfWidth));
            sink->AddLine(D2D1::Point2F(base.x + uy * halfWidth, base.y - ux * halfWidth));
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->Close();
            target->FillGeometry(geometry.Get(), brush.Get());
        }
        break;
    }
    case AnnotationSpec::Kind::Text:
        target->DrawTextLayout(D2D1::Point2F(left, top), textLayout.Get(), brush.Get());
        break;
    }
    if (FAILED(target->EndDraw())) return {};

    auto image = std::make_shared<DecodedImage>();
    image->width = width;
    image->height = height;
    image->pixels.resize(static_cast<size_t>(width) * height * 4);
    if (FAILED(wicBitmap->CopyPixels(nullptr, width * 4,
                                     static_cast<UINT>(image->pixels.size()),
                                     image->pixels.data()))) {
        return {};
    }
    return {std::move(image), originX, originY};
}

} // namespace blinker
