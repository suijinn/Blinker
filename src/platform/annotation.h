#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "core/geometry.h"
#include "platform/decoder.h"

namespace blinker {

// 画像へ描き込む注釈(図形・テキスト)1件の指定。座標・太さはすべて画像座標。
struct AnnotationSpec {
    enum class Kind { Rect, Ellipse, Arrow, Line, Text };
    Kind kind = Kind::Rect;
    Point p1;               // Rect/Ellipse/Text は対角の一方、Arrow/Line は始点
    Point p2;               // Rect/Ellipse/Text は対角の他方、Arrow/Line は終点
    uint32_t colorRGB = 0;
    float strokeWidth = 1;
    float angleDeg = 0;     // バウンディングボックス中心周りの回転(時計回り、度)
    float fontSize = 16;    // Text 用
    std::wstring text;      // Text 用(改行 '\n' 可)
};

// ラスタライズ結果。image はバウンディングボックス分の PBGRA で、
// 合成先画像の (x, y) へ blendOverlay で重ねる。
struct AnnotationOverlay {
    std::shared_ptr<DecodedImage> image;  // 失敗時は nullptr
    int x = 0;
    int y = 0;
};

// 注釈ラスタライズのプラットフォーム抽象。Windows 実装は D2D/DirectWrite (annotation_d2d)。
// UI スレッドからのみ呼ばれる。
class IAnnotationRasterizer {
public:
    virtual ~IAnnotationRasterizer() = default;
    virtual AnnotationOverlay rasterize(const AnnotationSpec& spec) = 0;
};

} // namespace blinker
