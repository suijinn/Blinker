#pragma once

#include <windows.h>

#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <list>
#include <utility>

#include "platform/renderer.h"

/**
 * @file renderer_d2d.h
 * @brief Direct2D による描画(Windows 実装)。
 */

namespace blinker {

/**
 * @brief ID2D1HwndRenderTarget による描画。
 *
 * 文字(ステータスバー・サイドバー)は DirectWrite で描く。UI スレッド専用。
 */
class RendererD2D final : public IRenderer {
public:
    /**
     * @brief 描画先ウィンドウを指定して構築する。
     * @param[in] hwnd 描画先のウィンドウハンドル。本オブジェクトより長生きすること。
     */
    explicit RendererD2D(HWND hwnd);

    /**
     * @brief 描画先のサイズ変更を通知する。
     * @param[in] width  新しい幅(物理ピクセル)。
     * @param[in] height 新しい高さ(物理ピクセル)。
     */
    void resize(uint32_t width, uint32_t height) override;

    /**
     * @brief 1 フレームを描画する。
     * @param[in] image         表示する画像。nullptr 可(背景のみ描画)。
     * @param[in] imageToScreen 画像座標 → スクリーン座標の変換行列。
     * @param[in] zoom          ズーム倍率。補間モード選択のヒントに使う。
     * @param[in] backgroundRGB 背景色(0xRRGGBB)。
     * @param[in] annotations   注釈オブジェクトの描画内容。
     * @param[in] selection     選択領域(ラバーバンド)の描画内容。
     * @param[in] navArrows     画像遷移用オーバーレイ矢印の描画内容。
     * @param[in] sidebar       サイドバーの描画内容。
     * @param[in] statusBar     ステータスバーの描画内容。
     */
    void render(const std::shared_ptr<const DecodedImage>& image,
                const Matrix3x2& imageToScreen, float zoom, uint32_t backgroundRGB,
                const AnnotationsView& annotations, const SelectionView& selection,
                const NavArrowsView& navArrows, const SidebarView& sidebar,
                const StatusBarView& statusBar) override;

private:
    /**
     * @brief レンダーターゲットを必要なら生成する。
     * @return 使用可能な状態なら true。生成に失敗したら false。
     */
    bool ensureTarget();

    /// @brief レンダーターゲットと依存リソース(ブラシ・ビットマップ)を破棄する。
    void discardTarget();

    /**
     * @brief デコード画像に対応する GPU ビットマップを取得する(なければ生成)。
     *
     * 生成そのものは `createD2DBitmap`(上限を超える画像は縮小して載せる)。ここは
     * その結果を小さな LRU に溜め、毎フレームの転送を避ける役目を持つ。表示中の画像と
     * Image 注釈の画素の両方がここを通る。縮小するのは GPU 側のコピーだけで、
     * 保存・コピー・文字認識は元の大きさで行われる。
     *
     * @param[in] image 元になるデコード画像。
     * @return GPU ビットマップ。所有権は bitmaps_ 側に残る。生成失敗時は nullptr。
     *         画像より小さいことがあるので、描画時の転送元矩形はこのビットマップの
     *         大きさ (`GetSize`) から取ること。
     */
    ID2D1Bitmap* bitmapFor(const std::shared_ptr<const DecodedImage>& image);

    /**
     * @brief 注釈オブジェクトと選択枠・ハンドルを描く。
     * @param[in] annotations   描画する注釈の内容。
     * @param[in] imageToScreen 画像座標 → スクリーン座標の変換行列。
     */
    void drawAnnotations(const AnnotationsView& annotations, const Matrix3x2& imageToScreen);

    /**
     * @brief 選択領域(ラバーバンド)を描く。
     * @param[in] selection 描画する選択領域。
     */
    void drawSelection(const SelectionView& selection);

    /**
     * @brief 画像遷移用オーバーレイ矢印を描く。
     * @param[in] navArrows 描画する矢印の内容。visible なボタンだけ描く。
     * @note 廃止しうる表示のため、必要なリソース(山形用の丸い線種)もここで遅延生成する。
     */
    void drawNavArrows(const NavArrowsView& navArrows);

    /**
     * @brief サイドバーを描く。
     * @param[in] sidebar 描画するサイドバーの内容。
     */
    void drawSidebar(const SidebarView& sidebar);

    /**
     * @brief ステータスバーを描く。
     * @param[in] bar 描画するステータスバーの内容。
     */
    void drawStatusBar(const StatusBarView& bar);

    HWND hwnd_;
    Microsoft::WRL::ComPtr<ID2D1Factory> factory_;
    Microsoft::WRL::ComPtr<IDWriteFactory> dwriteFactory_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> textFormat_;
    Microsoft::WRL::ComPtr<ID2D1StrokeStyle> dashStroke_;  ///< 選択枠の破線
    /// オーバーレイ矢印の山形用(端と角が丸い実線)。初回描画時に作る
    Microsoft::WRL::ComPtr<ID2D1StrokeStyle> navGlyphStroke_;
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> target_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_;  ///< target_ と同寿命
    /// @brief GPU ビットマップキャッシュ 1 件分。
    struct BitmapEntry {
        std::shared_ptr<const DecodedImage> image;      ///< キー(内容ではなく同一性で照合)
        Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;     ///< GPU 側のコピー
        size_t bytes = 0;                               ///< bitmap のおおよそのバイト数
    };
    /// 直近使用したデコード画像の GPU ビットマップ(小さな LRU)。
    /// shared_ptr をキーに持つことで CPU 側画像の解放とアドレス再利用による取り違えを防ぐ。
    /// 枚数とバイト数の両方で上限を設ける(巨大画像 3 枚で GPU メモリを使い切らないため)
    std::list<BitmapEntry> bitmaps_;
};

} // namespace blinker
