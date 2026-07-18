#pragma once

#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include "platform/annotation.h"

namespace blinker {

// 図形・テキストを D2D/DirectWrite で WIC ビットマップへ描画し、
// PBGRA の DecodedImage として返す IAnnotationRasterizer 実装。UI スレッド専用。
class AnnotationD2D final : public IAnnotationRasterizer {
public:
    AnnotationD2D();

    AnnotationOverlay rasterize(const AnnotationSpec& spec) override;

private:
    Microsoft::WRL::ComPtr<ID2D1Factory> factory_;
    Microsoft::WRL::ComPtr<IDWriteFactory> dwriteFactory_;
};

} // namespace blinker
