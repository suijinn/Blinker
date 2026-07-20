#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

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
 * @brief Text 注釈内のキャレット位置。
 *
 * 座標はテキストのバウンディングボックス左上を原点とする画像座標
 * (回転前。回転は描画側が spec.angleDeg で合わせる)。
 */
struct TextCaretMetrics {
    float x = 0;       ///< キャレットの X 位置
    float y = 0;       ///< キャレットの上端 Y 位置
    float height = 0;  ///< キャレットの高さ(その行の高さ)
};

/**
 * @brief Text 注釈内の矩形(選択範囲のハイライト 1 行分)。
 *
 * 座標系は TextCaretMetrics と同じ。
 */
struct TextRangeRect {
    float left = 0;    ///< 左端 X
    float top = 0;     ///< 上端 Y
    float right = 0;   ///< 右端 X
    float bottom = 0;  ///< 下端 Y
};

/**
 * @brief 注釈のラスタライズとテキスト計測のプラットフォーム抽象。
 *
 * Windows 実装は D2D/DirectWrite (annotation_d2d)。UI スレッドからのみ呼ばれる。
 * 計測系のメソッドは Text 注釈のインプレース編集(キャレット描画・クリック位置の
 * 特定)に使う。位置指定は UTF-16 コード単位で、UTF-8 との変換は
 * core/unicode.h の utf8ToUtf16Offset / utf16ToUtf8Offset で行う。
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

    /**
     * @brief Text 注釈内のキャレット位置を求める。
     * @param[in] spec        対象の Text 注釈。
     * @param[in] utf16Offset キャレットのテキスト内位置(UTF-16 コード単位)。
     * @return キャレットの位置と高さ。計測に失敗したら height = 0。
     */
    virtual TextCaretMetrics caretMetrics(const AnnotationSpec& spec, size_t utf16Offset) = 0;

    /**
     * @brief Text 注釈内の座標に対応するテキスト位置を求める(クリック位置の特定)。
     * @param[in] spec   対象の Text 注釈。
     * @param[in] localX バウンディングボックス左上を原点とする X(画像座標)。
     * @param[in] localY 同じく Y。
     * @return 最も近い文字境界の位置(UTF-16 コード単位)。計測に失敗したら 0。
     */
    virtual size_t hitTestTextOffset(const AnnotationSpec& spec, float localX,
                                     float localY) = 0;

    /**
     * @brief Text 注釈内の範囲を覆う矩形を求める(選択範囲のハイライト用)。
     * @param[in] spec        対象の Text 注釈。
     * @param[in] utf16Begin  範囲の開始位置(UTF-16 コード単位)。
     * @param[in] utf16End    範囲の終了位置(UTF-16 コード単位)。
     * @return 折り返しで分かれた行ごとの矩形。範囲が空・失敗時は空の vector。
     */
    virtual std::vector<TextRangeRect> selectionRects(const AnnotationSpec& spec,
                                                      size_t utf16Begin, size_t utf16End) = 0;
};

} // namespace blinker
