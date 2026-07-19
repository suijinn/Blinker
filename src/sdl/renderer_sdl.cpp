#include "sdl/renderer_sdl.h"

#include <algorithm>
#include <cmath>
#include <string_view>

namespace blinker {

RendererSdl::RendererSdl(SDL_Renderer* renderer, FontStb& font)
    : renderer_(renderer), font_(font) {}

RendererSdl::~RendererSdl() {
    for (CacheEntry& entry : cache_) {
        if (entry.texture) SDL_DestroyTexture(entry.texture);
    }
}

void RendererSdl::resize(uint32_t, uint32_t) {}

SDL_Texture* RendererSdl::textureFor(const std::shared_ptr<const DecodedImage>& image) {
    // shared_ptr をキーにすることでアドレス再利用による取り違えを防ぐ(D2D 実装と同じ)
    for (size_t i = 0; i < cache_.size(); ++i) {
        if (cache_[i].image == image) {
            std::rotate(cache_.begin(), cache_.begin() + i, cache_.begin() + i + 1);
            return cache_.front().texture;
        }
    }
    SDL_Texture* texture = SDL_CreateTexture(
        renderer_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC,
        static_cast<int>(image->width), static_cast<int>(image->height));
    if (!texture) return nullptr;
    SDL_UpdateTexture(texture, nullptr, image->pixels.data(),
                      static_cast<int>(image->width) * 4);
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND_PREMULTIPLIED);
    cache_.insert(cache_.begin(), {image, texture});
    while (cache_.size() > kTextureCacheSize) {
        if (cache_.back().texture) SDL_DestroyTexture(cache_.back().texture);
        cache_.pop_back();
    }
    return texture;
}

void RendererSdl::fillRect(float x, float y, float w, float h, uint32_t rgb, uint8_t alpha) {
    SDL_SetRenderDrawBlendMode(renderer_,
                               alpha == 255 ? SDL_BLENDMODE_NONE : SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer_, static_cast<Uint8>((rgb >> 16) & 0xFF),
                           static_cast<Uint8>((rgb >> 8) & 0xFF),
                           static_cast<Uint8>(rgb & 0xFF), alpha);
    const SDL_FRect rect{x, y, w, h};
    SDL_RenderFillRect(renderer_, &rect);
}

void RendererSdl::drawImage(const std::shared_ptr<const DecodedImage>& image,
                            const Matrix3x2& imageToScreen, float zoom) {
    SDL_Texture* texture = textureFor(image);
    if (!texture) return;
    SDL_SetTextureScaleMode(texture, zoom >= 4.0f ? SDL_SCALEMODE_NEAREST
                                                  : SDL_SCALEMODE_LINEAR);
    // 回転(90度単位)を含む変換のため、四隅を変換して 2 枚の三角形で描く
    const float w = static_cast<float>(image->width);
    const float h = static_cast<float>(image->height);
    const Point corners[4] = {
        imageToScreen.apply({0, 0}),
        imageToScreen.apply({w, 0}),
        imageToScreen.apply({w, h}),
        imageToScreen.apply({0, h}),
    };
    const SDL_FColor white{1.0f, 1.0f, 1.0f, 1.0f};
    const SDL_Vertex vertices[4] = {
        {{corners[0].x, corners[0].y}, white, {0, 0}},
        {{corners[1].x, corners[1].y}, white, {1, 0}},
        {{corners[2].x, corners[2].y}, white, {1, 1}},
        {{corners[3].x, corners[3].y}, white, {0, 1}},
    };
    const int indices[6] = {0, 1, 2, 0, 2, 3};
    SDL_RenderGeometry(renderer_, texture, vertices, 4, indices, 6);
}

void RendererSdl::drawSelection(const SelectionView& selection) {
    if (!selection.visible) return;
    const float left = std::min(selection.p1.x, selection.p2.x);
    const float top = std::min(selection.p1.y, selection.p2.y);
    const float w = std::abs(selection.p2.x - selection.p1.x);
    const float h = std::abs(selection.p2.y - selection.p1.y);
    fillRect(left, top, w, h, selection.fillARGB & 0xFFFFFF,
             static_cast<uint8_t>((selection.fillARGB >> 24) & 0xFF));
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(renderer_, static_cast<Uint8>((selection.borderRGB >> 16) & 0xFF),
                           static_cast<Uint8>((selection.borderRGB >> 8) & 0xFF),
                           static_cast<Uint8>(selection.borderRGB & 0xFF), 255);
    const SDL_FRect rect{left, top, w, h};
    SDL_RenderRect(renderer_, &rect);
}

void RendererSdl::drawTextClipped(std::string_view utf8, float x, float top, float maxWidth,
                                  uint32_t rgb) {
    if (utf8.empty() || maxWidth <= 1 || !font_.ok()) return;
    const float textWidth = font_.measure(utf8, kUiFontHeight);
    const int bufWidth = static_cast<int>(std::ceil(std::min(textWidth, maxWidth)));
    const int bufHeight = static_cast<int>(std::ceil(font_.lineHeight(kUiFontHeight)));
    if (bufWidth <= 0 || bufHeight <= 0) return;

    std::vector<uint8_t> pixels(static_cast<size_t>(bufWidth) * bufHeight * 4, 0);
    font_.drawText(pixels.data(), bufWidth, bufHeight, bufWidth * 4, 0, 0, utf8,
                   kUiFontHeight, rgb);
    SDL_Surface* surface = SDL_CreateSurfaceFrom(bufWidth, bufHeight,
                                                 SDL_PIXELFORMAT_ARGB8888, pixels.data(),
                                                 bufWidth * 4);
    if (!surface) return;
    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer_, surface);
    SDL_DestroySurface(surface);
    if (!texture) return;
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND_PREMULTIPLIED);
    const SDL_FRect dst{x, top, static_cast<float>(bufWidth), static_cast<float>(bufHeight)};
    SDL_RenderTexture(renderer_, texture, nullptr, &dst);
    SDL_DestroyTexture(texture);
}

void RendererSdl::drawSidebar(const SidebarView& sidebar) {
    if (!sidebar.visible || sidebar.width <= 0) return;
    fillRect(0, 0, sidebar.width, sidebar.height, sidebar.backgroundRGB);

    constexpr float kPadding = 8.0f;
    constexpr float kScrollbarWidth = 4.0f;
    const float textTop = (sidebar.itemHeight - font_.lineHeight(kUiFontHeight)) / 2;
    for (size_t i = 0; i < sidebar.items.size(); ++i) {
        const SidebarItem& item = sidebar.items[i];
        const float top = sidebar.firstItemY + static_cast<float>(i) * sidebar.itemHeight;
        if (item.current) {
            fillRect(0, top, sidebar.width, sidebar.itemHeight, sidebar.currentBackgroundRGB);
        }
        drawTextClipped(item.text, kPadding, top + textTop,
                        sidebar.width - kPadding * 2 - kScrollbarWidth,
                        item.current ? sidebar.currentTextRGB : sidebar.textRGB);
    }

    if (sidebar.contentHeight > sidebar.height) {
        const float ratio = sidebar.height / sidebar.contentHeight;
        const float thumbHeight = std::max(20.0f, sidebar.height * ratio);
        const float range = sidebar.contentHeight - sidebar.height;
        const float thumbTop = (sidebar.scrollOffset / range) * (sidebar.height - thumbHeight);
        fillRect(sidebar.width - kScrollbarWidth, thumbTop, kScrollbarWidth, thumbHeight,
                 sidebar.scrollbarRGB);
    }
}

void RendererSdl::drawStatusBar(const StatusBarView& bar) {
    if (!bar.visible || bar.height <= 0) return;
    int outputW = 0, outputH = 0;
    SDL_GetCurrentRenderOutputSize(renderer_, &outputW, &outputH);
    const float width = static_cast<float>(outputW);
    const float top = static_cast<float>(outputH) - bar.height;
    fillRect(0, top, width, bar.height, bar.backgroundRGB);

    constexpr float kPadding = 8.0f;
    const float textTop = top + (bar.height - font_.lineHeight(kUiFontHeight)) / 2;
    float rightStart = width - kPadding;
    if (!bar.rightText.empty()) {
        const float textWidth = font_.measure(bar.rightText, kUiFontHeight);
        rightStart = width - kPadding - textWidth;
        drawTextClipped(bar.rightText, rightStart, textTop, textWidth + 1, bar.textRGB);
    }
    if (!bar.leftText.empty() && rightStart > kPadding * 2) {
        drawTextClipped(bar.leftText, kPadding, textTop, rightStart - kPadding * 2,
                        bar.textRGB);
    }
}

void RendererSdl::render(const std::shared_ptr<const DecodedImage>& image,
                         const Matrix3x2& imageToScreen, float zoom, uint32_t backgroundRGB,
                         const AnnotationsView& annotations, const SelectionView& selection,
                         const SidebarView& sidebar, const StatusBarView& statusBar) {
    SDL_SetRenderDrawColor(renderer_, static_cast<Uint8>((backgroundRGB >> 16) & 0xFF),
                           static_cast<Uint8>((backgroundRGB >> 8) & 0xFF),
                           static_cast<Uint8>(backgroundRGB & 0xFF), 255);
    SDL_RenderClear(renderer_);
    if (image) drawImage(image, imageToScreen, zoom);
    // 注釈オブジェクトの描画は SDL バックエンドでは未対応(編集メニューが未実装のため
    // 注釈は作られない)。annotations は将来の実装のためのプレースホルダ
    (void)annotations;
    drawSelection(selection);
    drawSidebar(sidebar);
    drawStatusBar(statusBar);
    SDL_RenderPresent(renderer_);
}

} // namespace blinker
