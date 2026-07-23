#pragma once

#include "platform/annotation.h"

/**
 * @file annotation_stub.h
 * @brief 注釈ラスタライザのスタブ(SDL バックエンド用)。
 */

namespace blinker {

/**
 * @brief 常に失敗を返す IAnnotationRasterizer 実装。
 *
 * SDL バックエンドは注釈編集を未サポート(コンテキストメニュー未実装のため
 * 編集メニューに到達しない)。万一呼ばれても失敗を返し、App 側は
 * 「描画に失敗しました」の通知で安全に継続する。
 *
 * @todo SDL バックエンドで注釈編集に対応する。stb_truetype による描画は
 *       FontStb にあるため、図形描画とテキストのラスタライズを実装すれば置き換えられる。
 */
class AnnotationStub final : public IAnnotationRasterizer {
public:
    /**
     * @brief 注釈のラスタライズ(常に失敗)。
     * @return image が nullptr の AnnotationOverlay。
     */
    AnnotationOverlay rasterize(const AnnotationSpec&) override { return {}; }

    /**
     * @brief キャレット位置の計測(常に失敗)。
     * @return height = 0 の TextCaretMetrics。
     */
    TextCaretMetrics caretMetrics(const AnnotationSpec&, size_t) override { return {}; }

    /**
     * @brief 座標からの文字位置の特定(常に先頭)。
     * @return 常に 0。
     */
    size_t hitTestTextOffset(const AnnotationSpec&, float, float) override { return 0; }

    /**
     * @brief 選択範囲の矩形の計測(常に空)。
     * @return 常に空の vector。
     */
    std::vector<TextRangeRect> selectionRects(const AnnotationSpec&, size_t, size_t) override {
        return {};
    }

    /**
     * @brief フォントの有無の問い合わせ(常に無し)。
     * @return 常に false。
     */
    bool hasFontFamily(const std::string&) override { return false; }
};

} // namespace blinker
