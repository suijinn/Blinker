#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/geometry.h"
#include "platform/annotation.h"
#include "platform/decoder.h"

/**
 * @file renderer.h
 * @brief 描画のプラットフォーム抽象と、App が組み立てる描画用スナップショット。
 */

namespace blinker {

/**
 * @brief ステータスバーの描画内容。
 *
 * App が文字列まで組み立て、レンダラは描くだけ。
 */
struct StatusBarView {
    bool visible = false;        ///< 表示するか
    float height = 0;            ///< 高さ(px)
    uint32_t backgroundRGB = 0;  ///< 背景色(0xRRGGBB)
    uint32_t textRGB = 0;        ///< 文字色(0xRRGGBB)
    std::string leftText;        ///< 通知メッセージ or 画像情報(UTF-8)
    std::string rightText;       ///< カーソル位置の座標と色(UTF-8)
};

/// @brief サイドバー(ファイル名一覧)の 1 項目。
struct SidebarItem {
    std::string text;      ///< 表示するファイル名(UTF-8)
    bool current = false;  ///< 表示中(一覧の現在位置)ならハイライトする
};

/**
 * @brief サイドバー(ファイル名一覧)の描画内容。
 *
 * App が可視範囲の項目だけを組み立てる。領域はウィンドウ左端 (0, 0)-(width, height) で、
 * ステータスバーの高さは含まない。
 */
struct SidebarView {
    bool visible = false;               ///< 表示するか
    float width = 0;                    ///< 幅(px)
    float height = 0;                   ///< 高さ(px)。ステータスバーを含まない
    float itemHeight = 0;               ///< 1 項目の高さ(px)
    float firstItemY = 0;               ///< items[0] の上端 Y。スクロールの端数を含むため負になりうる
    uint32_t backgroundRGB = 0;         ///< 背景色(0xRRGGBB)
    uint32_t textRGB = 0;               ///< 文字色(0xRRGGBB)
    uint32_t currentBackgroundRGB = 0;  ///< 現在項目の背景色(0xRRGGBB)
    uint32_t currentTextRGB = 0;        ///< 現在項目の文字色(0xRRGGBB)
    uint32_t scrollbarRGB = 0;          ///< スクロールバーの色(0xRRGGBB)
    float scrollOffset = 0;             ///< スクロールバー描画用のスクロール量(px)
    float contentHeight = 0;            ///< 全項目の合計高さ。height 以下ならスクロールバー不要
    std::vector<SidebarItem> items;     ///< 可視範囲の項目のみ
};

/**
 * @brief Text 注釈をインプレース編集中のキャレット・選択範囲の描画内容。
 *
 * 座標は画像座標(注釈本体と同じ)。レンダラは編集対象の注釈と同じ変換
 * (imageToScreen と angleDeg の回転)を適用して描く。
 */
struct TextEditView {
    bool active = false;   ///< 編集中か。false なら以下は無効
    size_t index = 0;      ///< 編集中の注釈の index(specs 内)
    bool caretVisible = false;  ///< キャレットの点滅の表示相。false の間は描かない
    Point caretTop;        ///< キャレット上端(画像座標)
    Point caretBottom;     ///< キャレット下端(画像座標)
    std::vector<TextRangeRect> selectionRects;  ///< 選択範囲のハイライト(画像座標)
    /// IME 変換中文字列の範囲(画像座標)。細い下線を引く。空なら変換中でない
    std::vector<TextRangeRect> compositionRects;
    /// 変換対象の節の範囲(画像座標)。太い下線を引く。無ければ空
    std::vector<TextRangeRect> compositionTargetRects;
    uint32_t caretRGB = 0;        ///< キャレットの色(0xRRGGBB)
    uint32_t selectionARGB = 0;   ///< 選択ハイライトの色。上位バイト = アルファ
};

/**
 * @brief 注釈オブジェクトのライブ描画内容。
 *
 * specs は画像座標のまま渡し、レンダラが imageToScreen を適用して描く
 * (保存/コピー時の焼き込みと同じ見た目になる)。
 */
struct AnnotationsView {
    const std::vector<AnnotationSpec>* specs = nullptr;  ///< 注釈一覧。nullptr なら注釈なし
    std::optional<size_t> selected;  ///< 選択中の index(選択枠とハンドルを描く)
    uint32_t selectionRGB = 0;       ///< 選択枠・回転ハンドルの色(0xRRGGBB)
    float handleOffsetPx = 0;        ///< 回転ハンドルの枠上辺からの距離(画面px)
    float handleRadiusPx = 0;        ///< 回転ハンドルの半径(画面px)
    float resizeHandleSizePx = 0;    ///< サイズ変更ハンドル(正方形)の一辺(画面px)
    TextEditView textEdit;           ///< インプレース編集中のキャレット・選択範囲
    /// 右ドラッグ中のプレビュー(まだ specs には入っていない仮の注釈)。nullptr なら無し。
    /// specs と同じ見た目で描き、選択枠・ハンドルは付けない
    const AnnotationSpec* preview = nullptr;
};

/**
 * @brief 編集用の選択領域(ラバーバンド)の描画内容。
 *
 * App が画像座標 → スクリーン座標へ変換済み。
 */
struct SelectionView {
    bool visible = false;   ///< 表示するか
    Point p1;               ///< 矩形の対角の一方(順不同)
    Point p2;               ///< 矩形の対角の他方(順不同)
    uint32_t borderRGB = 0; ///< 枠線の色(0xRRGGBB)
    uint32_t fillARGB = 0;  ///< 塗りの色。上位バイト = アルファ
};

/**
 * @brief 描画のプラットフォーム抽象。
 *
 * Windows 実装は Direct2D (renderer_d2d)、SDL バックエンドは SDL_Renderer (renderer_sdl)。
 * UI スレッドからのみ呼ばれる。
 */
class IRenderer {
public:
    virtual ~IRenderer() = default;

    /**
     * @brief 描画先のサイズ変更を通知する。
     * @param[in] width  新しい幅(物理ピクセル)。
     * @param[in] height 新しい高さ(物理ピクセル)。
     */
    virtual void resize(uint32_t width, uint32_t height) = 0;

    /**
     * @brief 1 フレームを描画する。
     * @param[in] image           表示する画像。nullptr 可(背景のみ描画)。
     * @param[in] imageToScreen   画像座標 → スクリーン座標の変換行列。
     * @param[in] zoom            ズーム倍率。補間モード選択のヒントに使う。
     * @param[in] backgroundRGB   背景色(0xRRGGBB)。
     * @param[in] annotations     注釈オブジェクトの描画内容。
     * @param[in] selection       選択領域(ラバーバンド)の描画内容。
     * @param[in] sidebar         サイドバーの描画内容。
     * @param[in] statusBar       ステータスバーの描画内容。
     */
    virtual void render(const std::shared_ptr<const DecodedImage>& image,
                        const Matrix3x2& imageToScreen, float zoom, uint32_t backgroundRGB,
                        const AnnotationsView& annotations, const SelectionView& selection,
                        const SidebarView& sidebar, const StatusBarView& statusBar) = 0;
};

} // namespace blinker
