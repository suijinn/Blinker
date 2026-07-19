#pragma once

#include <SDL3/SDL.h>

#include <memory>
#include <vector>

#include "platform/renderer.h"
#include "sdl/font_stb.h"

namespace blinker {

// SDL_Renderer による描画(SDL バックエンド用)。画像は BGRA テクスチャへ
// アップロードし(直近数枚をキャッシュ)、UI テキストは FontStb で CPU 合成した
// 小さなテクスチャを都度作って描く。UI スレッドからのみ呼ばれる。
class RendererSdl final : public IRenderer {
public:
    RendererSdl(SDL_Renderer* renderer, FontStb& font);
    ~RendererSdl() override;

    void resize(uint32_t width, uint32_t height) override;  // SDL は自動追従のため何もしない
    void render(const std::shared_ptr<const DecodedImage>& image,
                const Matrix3x2& imageToScreen, float zoom, uint32_t backgroundRGB,
                const AnnotationsView& annotations, const SelectionView& selection,
                const SidebarView& sidebar, const StatusBarView& statusBar) override;

private:
    static constexpr size_t kTextureCacheSize = 3;  // 表示中 + 前後の先読み分
    static constexpr float kUiFontHeight = 13.0f;   // RendererD2D の 13px と同じ

    SDL_Texture* textureFor(const std::shared_ptr<const DecodedImage>& image);
    void drawImage(const std::shared_ptr<const DecodedImage>& image,
                   const Matrix3x2& imageToScreen, float zoom);
    void drawSelection(const SelectionView& selection);
    void drawSidebar(const SidebarView& sidebar);
    void drawStatusBar(const StatusBarView& statusBar);
    // (x, top) から最大 maxWidth px までテキストを 1 行描く
    void drawTextClipped(std::string_view utf8, float x, float top, float maxWidth,
                         uint32_t rgb);
    void fillRect(float x, float y, float w, float h, uint32_t rgb, uint8_t alpha = 255);

    SDL_Renderer* renderer_ = nullptr;
    FontStb& font_;
    struct CacheEntry {
        std::shared_ptr<const DecodedImage> image;
        SDL_Texture* texture = nullptr;
    };
    std::vector<CacheEntry> cache_;  // 先頭が最近使用(LRU)
};

} // namespace blinker
