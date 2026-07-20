#pragma once

#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include "platform/annotation.h"

/**
 * @file annotation_d2d.h
 * @brief 注釈ラスタライザ(Windows 実装)。
 */

namespace blinker {

/**
 * @brief 図形・テキストを D2D/DirectWrite でラスタライズする IAnnotationRasterizer 実装。
 *
 * WIC ビットマップへ描画し、PBGRA の DecodedImage として返す。UI スレッド専用。
 */
class AnnotationD2D final : public IAnnotationRasterizer {
public:
    /// @brief D2D / DirectWrite のファクトリを生成する。
    AnnotationD2D();

    /**
     * @brief 注釈 1 件をラスタライズする。
     * @param[in] spec ラスタライズする注釈。
     * @return バウンディングボックス分の PBGRA 画像と合成先座標。
     *         ファクトリ生成失敗・描画失敗時は image が nullptr。
     */
    AnnotationOverlay rasterize(const AnnotationSpec& spec) override;

private:
    Microsoft::WRL::ComPtr<ID2D1Factory> factory_;
    Microsoft::WRL::ComPtr<IDWriteFactory> dwriteFactory_;
};

} // namespace blinker
