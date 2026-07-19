#include "win/renderer_d2d.h"

#include <algorithm>
#include <array>

#include "core/annotation_edit.h"
#include "win/annotation_draw.h"

namespace blinker {
namespace {

using Microsoft::WRL::ComPtr;

constexpr size_t kBitmapCacheSize = 3;

D2D1_COLOR_F colorFromRGB(uint32_t rgb) {
    return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f,
                        (rgb & 0xFF) / 255.0f);
}

} // namespace

RendererD2D::RendererD2D(HWND hwnd) : hwnd_(hwnd) {
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory_.GetAddressOf());
    if (SUCCEEDED(DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(dwriteFactory_.GetAddressOf())))) {
        dwriteFactory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                         DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                         13.0f, L"ja-jp", &textFormat_);
    }
    if (factory_) {
        // 選択枠用の破線。デバイス非依存リソースなので target_ 再作成の影響を受けない
        factory_->CreateStrokeStyle(
            D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
                                        D2D1_CAP_STYLE_FLAT, D2D1_LINE_JOIN_MITER, 10.0f,
                                        D2D1_DASH_STYLE_DASH, 0.0f),
            nullptr, 0, &dashStroke_);
    }
    if (textFormat_) {
        textFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        textFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);  // 上下中央
        // 収まらないテキスト(長いパス等)は末尾を "…" で切り詰める
        ComPtr<IDWriteInlineObject> ellipsis;
        if (SUCCEEDED(dwriteFactory_->CreateEllipsisTrimmingSign(textFormat_.Get(), &ellipsis))) {
            DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            textFormat_->SetTrimming(&trimming, ellipsis.Get());
        }
    }
}

bool RendererD2D::ensureTarget() {
    if (target_) return true;
    if (!factory_) return false;
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const D2D1_SIZE_U size = D2D1::SizeU(std::max<LONG>(rc.right - rc.left, 1),
                                         std::max<LONG>(rc.bottom - rc.top, 1));
    // DPI を 96 に固定し、DIP = 物理ピクセルとして扱う(座標系を全編ピクセルで統一)
    const auto props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.0f, 96.0f);
    if (FAILED(factory_->CreateHwndRenderTarget(
            props, D2D1::HwndRenderTargetProperties(hwnd_, size), &target_))) {
        return false;
    }
    target_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &brush_);
    return true;
}

void RendererD2D::discardTarget() {
    bitmaps_.clear();
    brush_.Reset();
    target_.Reset();
}

void RendererD2D::resize(uint32_t width, uint32_t height) {
    if (target_) {
        target_->Resize(D2D1::SizeU(std::max(width, 1u), std::max(height, 1u)));
    }
}

ID2D1Bitmap* RendererD2D::bitmapFor(const std::shared_ptr<const DecodedImage>& image) {
    for (auto it = bitmaps_.begin(); it != bitmaps_.end(); ++it) {
        if (it->first == image) {
            bitmaps_.splice(bitmaps_.begin(), bitmaps_, it);
            return bitmaps_.front().second.Get();
        }
    }
    ComPtr<ID2D1Bitmap> bitmap;
    const auto props = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(target_->CreateBitmap(D2D1::SizeU(image->width, image->height),
                                     image->pixels.data(), image->width * 4, props, &bitmap))) {
        return nullptr;
    }
    bitmaps_.emplace_front(image, std::move(bitmap));
    if (bitmaps_.size() > kBitmapCacheSize) bitmaps_.pop_back();
    return bitmaps_.front().second.Get();
}

void RendererD2D::render(const std::shared_ptr<const DecodedImage>& image,
                         const Matrix3x2& imageToScreen, float zoom, uint32_t backgroundRGB,
                         const AnnotationsView& annotations, const SelectionView& selection,
                         const SidebarView& sidebar, const StatusBarView& statusBar) {
    if (!ensureTarget()) return;
    target_->BeginDraw();
    target_->Clear(colorFromRGB(backgroundRGB));
    if (image && image->width > 0) {
        if (ID2D1Bitmap* bitmap = bitmapFor(image)) {
            target_->SetTransform(D2D1::Matrix3x2F(imageToScreen.m11, imageToScreen.m12,
                                                   imageToScreen.m21, imageToScreen.m22,
                                                   imageToScreen.dx, imageToScreen.dy));
            // 拡大表示時はピクセル境界が見えるよう最近傍補間へ切り替える
            const auto mode = zoom >= 4.0f ? D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR
                                           : D2D1_BITMAP_INTERPOLATION_MODE_LINEAR;
            const auto rect = D2D1::RectF(0, 0, static_cast<float>(image->width),
                                          static_cast<float>(image->height));
            target_->DrawBitmap(bitmap, rect, 1.0f, mode, rect);
            target_->SetTransform(D2D1::Matrix3x2F::Identity());
        }
    }
    drawAnnotations(annotations, imageToScreen);  // 注釈オブジェクトは画像の上に重ねる
    drawSelection(selection);  // 編集領域のラバーバンドは画像の上に重ねる
    drawSidebar(sidebar);      // 画像の後に描き、ズーム時のはみ出しを覆う(不透明)
    drawStatusBar(statusBar);
    if (target_->EndDraw() == D2DERR_RECREATE_TARGET) {
        discardTarget();  // 次回の render で作り直す
    }
}

void RendererD2D::drawAnnotations(const AnnotationsView& annotations,
                                  const Matrix3x2& imageToScreen) {
    if (!annotations.specs || annotations.specs->empty() || !brush_) return;
    const auto toD2D = [](const Matrix3x2& m) {
        return D2D1::Matrix3x2F(m.m11, m.m12, m.m21, m.m22, m.dx, m.dy);
    };
    // 画像座標のまま描き、変換で拡縮する(線幅・文字も拡縮され焼き込み結果と一致する)
    for (const AnnotationSpec& spec : *annotations.specs) {
        D2D1::Matrix3x2F transform = toD2D(imageToScreen);
        if (spec.angleDeg != 0) {
            const Point c = annotationCenter(spec);
            transform =
                D2D1::Matrix3x2F::Rotation(spec.angleDeg, D2D1::Point2F(c.x, c.y)) * transform;
        }
        target_->SetTransform(transform);
        brush_->SetColor(colorFromRGB(spec.colorRGB));
        drawAnnotationShape(target_.Get(), factory_.Get(), dwriteFactory_.Get(), spec,
                            brush_.Get());
    }
    target_->SetTransform(D2D1::Matrix3x2F::Identity());

    // 選択中オブジェクトの破線枠と回転ハンドル(スクリーン座標で等幅に描く)
    if (!annotations.selected || *annotations.selected >= annotations.specs->size()) return;
    const AnnotationSpec& spec = (*annotations.specs)[*annotations.selected];
    const std::array<Point, 4> corners = rotatedCorners(spec);
    std::array<D2D1_POINT_2F, 4> pts;
    for (size_t i = 0; i < 4; ++i) {
        const Point s = imageToScreen.apply(corners[i]);
        pts[i] = D2D1::Point2F(s.x, s.y);
    }
    brush_->SetColor(colorFromRGB(annotations.selectionRGB));
    for (size_t i = 0; i < 4; ++i) {
        target_->DrawLine(pts[i], pts[(i + 1) % 4], brush_.Get(), 1.0f, dashStroke_.Get());
    }
    const Point handle =
        rotationHandlePos(spec, imageToScreen, annotations.handleOffsetPx);
    const D2D1_POINT_2F mid{(pts[0].x + pts[1].x) * 0.5f, (pts[0].y + pts[1].y) * 0.5f};
    target_->DrawLine(mid, D2D1::Point2F(handle.x, handle.y), brush_.Get(), 1.0f,
                      dashStroke_.Get());
    target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(handle.x, handle.y),
                                       annotations.handleRadiusPx, annotations.handleRadiusPx),
                         brush_.Get());
}

void RendererD2D::drawSelection(const SelectionView& selection) {
    if (!selection.visible || !brush_) return;
    const auto rect = D2D1::RectF(
        std::min(selection.p1.x, selection.p2.x), std::min(selection.p1.y, selection.p2.y),
        std::max(selection.p1.x, selection.p2.x), std::max(selection.p1.y, selection.p2.y));
    const uint32_t fill = selection.fillARGB;
    brush_->SetColor(D2D1::ColorF(((fill >> 16) & 0xFF) / 255.0f, ((fill >> 8) & 0xFF) / 255.0f,
                                  (fill & 0xFF) / 255.0f, ((fill >> 24) & 0xFF) / 255.0f));
    target_->FillRectangle(rect, brush_.Get());
    brush_->SetColor(colorFromRGB(selection.borderRGB));
    target_->DrawRectangle(rect, brush_.Get(), 1.0f);
}

void RendererD2D::drawSidebar(const SidebarView& sidebar) {
    if (!sidebar.visible || sidebar.width <= 0 || !brush_ || !textFormat_) return;
    brush_->SetColor(colorFromRGB(sidebar.backgroundRGB));
    target_->FillRectangle(D2D1::RectF(0, 0, sidebar.width, sidebar.height), brush_.Get());

    constexpr float kPadding = 8.0f;
    constexpr float kScrollbarWidth = 4.0f;
    for (size_t i = 0; i < sidebar.items.size(); ++i) {
        const SidebarItem& item = sidebar.items[i];
        const float top = sidebar.firstItemY + static_cast<float>(i) * sidebar.itemHeight;
        const float bottom = top + sidebar.itemHeight;
        if (item.current) {
            brush_->SetColor(colorFromRGB(sidebar.currentBackgroundRGB));
            target_->FillRectangle(D2D1::RectF(0, top, sidebar.width, bottom), brush_.Get());
        }
        brush_->SetColor(colorFromRGB(item.current ? sidebar.currentTextRGB : sidebar.textRGB));
        const auto rect =
            D2D1::RectF(kPadding, top, sidebar.width - kPadding - kScrollbarWidth, bottom);
        target_->DrawText(item.text.c_str(), static_cast<UINT32>(item.text.size()),
                          textFormat_.Get(), rect, brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    // スクロールバー(内容が収まらないときだけ右端に表示)
    if (sidebar.contentHeight > sidebar.height) {
        const float ratio = sidebar.height / sidebar.contentHeight;
        const float thumbHeight = std::max(20.0f, sidebar.height * ratio);
        const float range = sidebar.contentHeight - sidebar.height;
        const float thumbTop =
            (sidebar.scrollOffset / range) * (sidebar.height - thumbHeight);
        brush_->SetColor(colorFromRGB(sidebar.scrollbarRGB));
        target_->FillRectangle(D2D1::RectF(sidebar.width - kScrollbarWidth, thumbTop,
                                           sidebar.width, thumbTop + thumbHeight),
                               brush_.Get());
    }
}

void RendererD2D::drawStatusBar(const StatusBarView& bar) {
    if (!bar.visible || bar.height <= 0 || !brush_ || !textFormat_) return;
    const D2D1_SIZE_F size = target_->GetSize();
    const float top = size.height - bar.height;
    brush_->SetColor(colorFromRGB(bar.backgroundRGB));
    target_->FillRectangle(D2D1::RectF(0, top, size.width, size.height), brush_.Get());

    constexpr float kPadding = 8.0f;
    brush_->SetColor(colorFromRGB(bar.textRGB));

    // 右側: 実測幅で右寄せ配置
    float rightStart = size.width - kPadding;
    if (!bar.rightText.empty()) {
        ComPtr<IDWriteTextLayout> layout;
        if (SUCCEEDED(dwriteFactory_->CreateTextLayout(
                bar.rightText.c_str(), static_cast<UINT32>(bar.rightText.size()),
                textFormat_.Get(), size.width, bar.height, &layout))) {
            DWRITE_TEXT_METRICS metrics{};
            layout->GetMetrics(&metrics);
            rightStart = size.width - kPadding - metrics.width;
            target_->DrawTextLayout(D2D1::Point2F(rightStart, top), layout.Get(), brush_.Get(),
                                    D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }

    // 左側: 右側テキストの手前まで。収まらなければ "…" で切り詰め
    if (!bar.leftText.empty() && rightStart > kPadding * 2) {
        const auto rect = D2D1::RectF(kPadding, top, rightStart - kPadding, size.height);
        target_->DrawText(bar.leftText.c_str(), static_cast<UINT32>(bar.leftText.size()),
                          textFormat_.Get(), rect, brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
}

} // namespace blinker
