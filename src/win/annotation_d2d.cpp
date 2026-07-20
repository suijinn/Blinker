#include "win/annotation_d2d.h"

#include <wincodec.h>

#include <algorithm>
#include <cmath>

#include "win/annotation_draw.h"
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

} // namespace

AnnotationD2D::AnnotationD2D() {
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory_.GetAddressOf());
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                        reinterpret_cast<IUnknown**>(dwriteFactory_.GetAddressOf()));
}

ComPtr<IDWriteTextLayout> AnnotationD2D::layoutForMetrics(const AnnotationSpec& spec) {
    if (!dwriteFactory_) return nullptr;
    // 空文字列だと行の高さが 0 になるため、空白 1 文字で行の高さを測る
    if (spec.text.empty()) {
        AnnotationSpec probe = spec;
        probe.text = " ";
        return createAnnotationTextLayout(dwriteFactory_.Get(), probe, kMaxOverlaySize);
    }
    return createAnnotationTextLayout(dwriteFactory_.Get(), spec, kMaxOverlaySize);
}

TextCaretMetrics AnnotationD2D::caretMetrics(const AnnotationSpec& spec, size_t utf16Offset) {
    const ComPtr<IDWriteTextLayout> layout = layoutForMetrics(spec);
    if (!layout) return {};
    float x = 0;
    float y = 0;
    DWRITE_HIT_TEST_METRICS metrics{};
    if (FAILED(layout->HitTestTextPosition(static_cast<UINT32>(utf16Offset), FALSE, &x, &y,
                                           &metrics))) {
        return {};
    }
    return {x, y, metrics.height};
}

size_t AnnotationD2D::hitTestTextOffset(const AnnotationSpec& spec, float localX,
                                        float localY) {
    if (spec.text.empty()) return 0;
    const ComPtr<IDWriteTextLayout> layout = layoutForMetrics(spec);
    if (!layout) return 0;
    BOOL isTrailing = FALSE;
    BOOL isInside = FALSE;
    DWRITE_HIT_TEST_METRICS metrics{};
    if (FAILED(layout->HitTestPoint(localX, localY, &isTrailing, &isInside, &metrics))) {
        return 0;
    }
    // 文字の後ろ半分を叩いたときは次の文字境界へ寄せる(通常のテキスト編集と同じ)
    return metrics.textPosition + (isTrailing ? metrics.length : 0);
}

std::vector<TextRangeRect> AnnotationD2D::selectionRects(const AnnotationSpec& spec,
                                                         size_t utf16Begin, size_t utf16End) {
    if (utf16End <= utf16Begin || spec.text.empty()) return {};
    const ComPtr<IDWriteTextLayout> layout = layoutForMetrics(spec);
    if (!layout) return {};
    const UINT32 position = static_cast<UINT32>(utf16Begin);
    const UINT32 length = static_cast<UINT32>(utf16End - utf16Begin);
    UINT32 count = 0;
    // 1 回目は必要な個数を得るための呼び出し(E_NOT_SUFFICIENT_BUFFER が返る)
    layout->HitTestTextRange(position, length, 0, 0, nullptr, 0, &count);
    if (count == 0) return {};
    std::vector<DWRITE_HIT_TEST_METRICS> hits(count);
    if (FAILED(layout->HitTestTextRange(position, length, 0, 0, hits.data(), count, &count))) {
        return {};
    }
    std::vector<TextRangeRect> rects;
    rects.reserve(count);
    for (UINT32 i = 0; i < count; ++i) {
        const DWRITE_HIT_TEST_METRICS& h = hits[i];
        rects.push_back({h.left, h.top, h.left + h.width, h.top + h.height});
    }
    return rects;
}

AnnotationOverlay AnnotationD2D::rasterize(const AnnotationSpec& spec) {
    IWICImagingFactory* wic = wicFactoryForThisThread();
    if (!factory_ || !dwriteFactory_ || !wic) return {};

    // テキストはレイアウトの実測、図形は端点+線幅からバウンディングボックスを決める
    BoundsF bounds{std::min(spec.p1.x, spec.p2.x), std::min(spec.p1.y, spec.p2.y),
                   std::max(spec.p1.x, spec.p2.x), std::max(spec.p1.y, spec.p2.y)};
    if (spec.kind == AnnotationSpec::Kind::Text) {
        const ComPtr<IDWriteTextLayout> layout =
            createAnnotationTextLayout(dwriteFactory_.Get(), spec, kMaxOverlaySize);
        if (!layout) return {};
        DWRITE_TEXT_METRICS metrics{};
        layout->GetMetrics(&metrics);
        bounds.maxX = bounds.minX + std::max(metrics.widthIncludingTrailingWhitespace, 1.0f);
        bounds.maxY = bounds.minY + std::max(metrics.height, 1.0f);
    }
    const float margin = spec.kind == AnnotationSpec::Kind::Text
                             ? 2.0f
                             : spec.strokeWidth + arrowHeadLength(spec.strokeWidth);
    // 回転はバウンディングボックス中心周り。マージン込みの矩形の四隅を回して
    // 回転後の AABB をオーバーレイの領域にする(回転は等長変換なのでマージンは保たれる)
    const float centerX = (bounds.minX + bounds.maxX) * 0.5f;
    const float centerY = (bounds.minY + bounds.maxY) * 0.5f;
    BoundsF outer{bounds.minX - margin, bounds.minY - margin, bounds.maxX + margin,
                  bounds.maxY + margin};
    if (spec.angleDeg != 0) {
        constexpr float kPi = 3.14159265358979323846f;
        const float rad = spec.angleDeg * kPi / 180.0f;
        const float c = std::cos(rad);
        const float s = std::sin(rad);
        const float hw = (outer.maxX - outer.minX) * 0.5f;
        const float hh = (outer.maxY - outer.minY) * 0.5f;
        // 中心対称なので回転後の半幅・半高は |c|,|s| の線形結合で求まる
        const float rw = std::abs(c) * hw + std::abs(s) * hh;
        const float rh = std::abs(s) * hw + std::abs(c) * hh;
        outer = {centerX - rw, centerY - rh, centerX + rw, centerY + rh};
    }
    const int originX = static_cast<int>(std::floor(outer.minX));
    const int originY = static_cast<int>(std::floor(outer.minY));
    const float widthF = std::ceil(outer.maxX) - static_cast<float>(originX);
    const float heightF = std::ceil(outer.maxY) - static_cast<float>(originY);
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

    target->BeginDraw();
    target->Clear(D2D1::ColorF(0, 0.0f));
    // 共通描画は画像座標で描くため、回転(bbox 中心周り)→ローカル座標への平行移動を設定
    D2D1::Matrix3x2F transform = D2D1::Matrix3x2F::Translation(
        -static_cast<float>(originX), -static_cast<float>(originY));
    if (spec.angleDeg != 0) {
        transform =
            D2D1::Matrix3x2F::Rotation(spec.angleDeg, D2D1::Point2F(centerX, centerY)) *
            transform;
    }
    target->SetTransform(transform);
    drawAnnotationShape(target.Get(), factory_.Get(), dwriteFactory_.Get(), spec, brush.Get());
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
