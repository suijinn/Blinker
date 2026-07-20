#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "core/geometry.h"
#include "platform/decoder.h"

/**
 * @file annotation.h
 * @brief 注釈(図形・テキスト)の指定と、ラスタライズのプラットフォーム抽象。
 */

namespace blinker {

/**
 * @brief 画像へ描き込む注釈(図形・テキスト)1 件の指定。
 *
 * 座標・太さはすべて画像座標。
 */
struct AnnotationSpec {
    /// @brief 注釈の種別。
    enum class Kind { Rect, Ellipse, Arrow, Line, Text };
    Kind kind = Kind::Rect;  ///< 注釈の種別
    Point p1;                ///< Rect/Ellipse/Text は対角の一方、Arrow/Line は始点
    Point p2;                ///< Rect/Ellipse/Text は対角の他方、Arrow/Line は終点
    uint32_t colorRGB = 0;   ///< 描画色(0xRRGGBB)
    float strokeWidth = 1;   ///< 線幅(画像座標)
    float angleDeg = 0;      ///< バウンディングボックス中心周りの回転(時計回り、度)
    float fontSize = 16;     ///< Text 用のフォントサイズ(画像座標)
    std::string text;        ///< Text 用の文字列(UTF-8。改行 LF 可)
};

/**
 * @brief 注釈のラスタライズ結果。
 *
 * image はバウンディングボックス分の PBGRA で、合成先画像の (x, y) へ
 * blendOverlay で重ねる。
 */
struct AnnotationOverlay {
    std::shared_ptr<DecodedImage> image;  ///< ラスタライズ結果。失敗時は nullptr
    int x = 0;                            ///< 合成先での左端 X 座標(画像座標)
    int y = 0;                            ///< 合成先での上端 Y 座標(画像座標)
};

/**
 * @brief 注釈ラスタライズのプラットフォーム抽象。
 *
 * Windows 実装は D2D/DirectWrite (annotation_d2d)。UI スレッドからのみ呼ばれる。
 */
class IAnnotationRasterizer {
public:
    virtual ~IAnnotationRasterizer() = default;

    /**
     * @brief 注釈 1 件をラスタライズする。
     * @param[in] spec ラスタライズする注釈。
     * @return ラスタライズ結果。失敗時は image が nullptr。
     */
    virtual AnnotationOverlay rasterize(const AnnotationSpec& spec) = 0;
};

} // namespace blinker
