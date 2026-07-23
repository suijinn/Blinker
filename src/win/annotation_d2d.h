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

    /**
     * @brief Text 注釈内のキャレット位置を DirectWrite の計測から求める。
     * @param[in] spec        対象の Text 注釈。
     * @param[in] utf16Offset キャレットのテキスト内位置(UTF-16 コード単位)。
     * @return キャレットの位置と高さ。レイアウト生成に失敗したら height = 0。
     */
    TextCaretMetrics caretMetrics(const AnnotationSpec& spec, size_t utf16Offset) override;

    /**
     * @brief Text 注釈内の座標に対応するテキスト位置を求める。
     * @param[in] spec   対象の Text 注釈。
     * @param[in] localX バウンディングボックス左上を原点とする X(画像座標)。
     * @param[in] localY 同じく Y。
     * @return 最も近い文字境界の位置(UTF-16 コード単位)。失敗時は 0。
     */
    size_t hitTestTextOffset(const AnnotationSpec& spec, float localX, float localY) override;

    /**
     * @brief Text 注釈内の範囲を覆う矩形を求める。
     * @param[in] spec       対象の Text 注釈。
     * @param[in] utf16Begin 範囲の開始位置(UTF-16 コード単位)。
     * @param[in] utf16End   範囲の終了位置(UTF-16 コード単位)。
     * @return 折り返しで分かれた行ごとの矩形。範囲が空・失敗時は空の vector。
     */
    std::vector<TextRangeRect> selectionRects(const AnnotationSpec& spec, size_t utf16Begin,
                                              size_t utf16End) override;

    /**
     * @brief フォントファミリがシステムフォントコレクションにあるかを返す。
     * @param[in] family 調べるフォントファミリ名(UTF-8)。
     * @return 見つかれば true。DirectWrite の初期化に失敗している場合は false。
     */
    bool hasFontFamily(const std::string& family) override;

private:
    /**
     * @brief 計測用のテキストレイアウトを作る。
     * @param[in] spec 対象の Text 注釈。
     * @return 生成されたレイアウト。失敗時は nullptr。
     * @note 空文字列は行の高さが 0 になるため、空白 1 文字に置き換えて測る。
     */
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layoutForMetrics(const AnnotationSpec& spec);

    Microsoft::WRL::ComPtr<ID2D1Factory> factory_;
    Microsoft::WRL::ComPtr<IDWriteFactory> dwriteFactory_;
};

} // namespace blinker
