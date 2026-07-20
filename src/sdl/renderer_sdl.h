#pragma once

#include <SDL3/SDL.h>

#include <memory>
#include <vector>

#include "platform/renderer.h"
#include "sdl/font_stb.h"

/**
 * @file renderer_sdl.h
 * @brief SDL_Renderer による描画(SDL バックエンド用)。
 */

namespace blinker {

/**
 * @brief SDL_Renderer による描画。
 *
 * 画像は BGRA テクスチャへアップロードし(直近数枚をキャッシュ)、UI テキストは
 * FontStb で CPU 合成した小さなテクスチャを都度作って描く。UI スレッドからのみ呼ばれる。
 *
 * @todo 注釈オブジェクトのライブ描画に未対応(render の annotations 引数を無視する)。
 *       SDL バックエンドは注釈編集自体が未サポートのため実害はないが、
 *       AnnotationStub を実装で置き換える際にはここも対応が必要。
 */
class RendererSdl final : public IRenderer {
public:
    /**
     * @brief 描画先とフォントを指定して構築する。
     * @param[in] renderer 描画先の SDL_Renderer。本オブジェクトより長生きすること。
     * @param[in] font     UI テキスト描画に使うフォント。本オブジェクトより長生きすること。
     */
    RendererSdl(SDL_Renderer* renderer, FontStb& font);

    /// @brief キャッシュしたテクスチャを破棄する。
    ~RendererSdl() override;

    /**
     * @brief 描画先のサイズ変更を通知する。
     * @param[in] width  新しい幅(物理ピクセル)。
     * @param[in] height 新しい高さ(物理ピクセル)。
     * @note SDL は描画先サイズに自動追従するため何もしない。
     */
    void resize(uint32_t width, uint32_t height) override;

    /**
     * @brief 1 フレームを描画する。
     * @param[in] image         表示する画像。nullptr 可(背景のみ描画)。
     * @param[in] imageToScreen 画像座標 → スクリーン座標の変換行列。
     * @param[in] zoom          ズーム倍率。補間モード選択のヒントに使う。
     * @param[in] backgroundRGB 背景色(0xRRGGBB)。
     * @param[in] annotations   注釈オブジェクトの描画内容(本実装では未使用)。
     * @param[in] selection     選択領域(ラバーバンド)の描画内容。
     * @param[in] sidebar       サイドバーの描画内容。
     * @param[in] statusBar     ステータスバーの描画内容。
     */
    void render(const std::shared_ptr<const DecodedImage>& image,
                const Matrix3x2& imageToScreen, float zoom, uint32_t backgroundRGB,
                const AnnotationsView& annotations, const SelectionView& selection,
                const SidebarView& sidebar, const StatusBarView& statusBar) override;

private:
    static constexpr size_t kTextureCacheSize = 3;  ///< 表示中 + 前後の先読み分
    static constexpr float kUiFontHeight = 13.0f;   ///< RendererD2D の 13px と同じ

    /**
     * @brief デコード画像に対応するテクスチャを取得する(なければ生成)。
     * @param[in] image 元になるデコード画像。
     * @return テクスチャ。所有権は cache_ 側に残る。生成失敗時は nullptr。
     */
    SDL_Texture* textureFor(const std::shared_ptr<const DecodedImage>& image);

    /**
     * @brief 画像を描く。
     * @param[in] image         描画する画像。
     * @param[in] imageToScreen 画像座標 → スクリーン座標の変換行列。
     * @param[in] zoom          ズーム倍率。補間モード選択のヒントに使う。
     */
    void drawImage(const std::shared_ptr<const DecodedImage>& image,
                   const Matrix3x2& imageToScreen, float zoom);

    /**
     * @brief 選択領域(ラバーバンド)を描く。
     * @param[in] selection 描画する選択領域。
     */
    void drawSelection(const SelectionView& selection);

    /**
     * @brief サイドバーを描く。
     * @param[in] sidebar 描画するサイドバーの内容。
     */
    void drawSidebar(const SidebarView& sidebar);

    /**
     * @brief ステータスバーを描く。
     * @param[in] statusBar 描画するステータスバーの内容。
     */
    void drawStatusBar(const StatusBarView& statusBar);

    /**
     * @brief テキストを 1 行、指定幅までで打ち切って描く。
     * @param[in] utf8     描画する文字列(UTF-8)。
     * @param[in] x        描画開始位置の左端 X 座標(px)。
     * @param[in] top      描画開始位置の上端 Y 座標(px)。
     * @param[in] maxWidth 描画できる最大幅(px)。これを超える分は描かない。
     * @param[in] rgb      文字色(0xRRGGBB)。
     */
    void drawTextClipped(std::string_view utf8, float x, float top, float maxWidth,
                         uint32_t rgb);

    /**
     * @brief 矩形を塗りつぶす。
     * @param[in] x     左端 X 座標(px)。
     * @param[in] y     上端 Y 座標(px)。
     * @param[in] w     幅(px)。
     * @param[in] h     高さ(px)。
     * @param[in] rgb   塗り色(0xRRGGBB)。
     * @param[in] alpha 不透明度(0 = 透明、255 = 不透明)。
     */
    void fillRect(float x, float y, float w, float h, uint32_t rgb, uint8_t alpha = 255);

    SDL_Renderer* renderer_ = nullptr;
    FontStb& font_;

    /// @brief テクスチャキャッシュ 1 件分のエントリ。
    struct CacheEntry {
        std::shared_ptr<const DecodedImage> image;  ///< 元のデコード画像(キーを兼ねる)
        SDL_Texture* texture = nullptr;             ///< 対応する GPU テクスチャ
    };
    std::vector<CacheEntry> cache_;  ///< 先頭が最近使用(LRU)
};

} // namespace blinker
