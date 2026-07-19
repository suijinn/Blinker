#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/geometry.h"
#include "platform/annotation.h"
#include "platform/decoder.h"

namespace blinker {

// ステータスバーの描画内容。App が文字列まで組み立て、レンダラは描くだけ。
struct StatusBarView {
    bool visible = false;
    float height = 0;
    uint32_t backgroundRGB = 0;
    uint32_t textRGB = 0;
    std::wstring leftText;   // 通知メッセージ or 画像情報
    std::wstring rightText;  // カーソル位置の座標と色
};

// サイドバー(ファイル名一覧)の描画内容。App が可視範囲の項目だけを組み立てる。
// 領域はウィンドウ左端 (0, 0)-(width, height)。ステータスバーの高さは含まない。
struct SidebarItem {
    std::wstring text;
    bool current = false;  // 表示中(一覧の現在位置)ならハイライト
};

struct SidebarView {
    bool visible = false;
    float width = 0;
    float height = 0;
    float itemHeight = 0;
    float firstItemY = 0;  // items[0] の上端 Y(スクロールの端数を含むため負になりうる)
    uint32_t backgroundRGB = 0;
    uint32_t textRGB = 0;
    uint32_t currentBackgroundRGB = 0;
    uint32_t currentTextRGB = 0;
    uint32_t scrollbarRGB = 0;
    float scrollOffset = 0;   // スクロールバー描画用
    float contentHeight = 0;  // 全項目の合計高さ。height 以下ならスクロールバー不要
    std::vector<SidebarItem> items;  // 可視範囲のみ
};

// 注釈オブジェクトのライブ描画内容。specs は画像座標のまま渡し、レンダラが
// imageToScreen を適用して描く(保存/コピー時の焼き込みと同じ見た目になる)。
struct AnnotationsView {
    const std::vector<AnnotationSpec>* specs = nullptr;  // nullptr = 注釈なし
    std::optional<size_t> selected;  // 選択中の index(選択枠とハンドルを描く)
    uint32_t selectionRGB = 0;       // 選択枠・回転ハンドルの色
    float handleOffsetPx = 0;        // 回転ハンドルの枠上辺からの距離(画面px)
    float handleRadiusPx = 0;        // 回転ハンドルの半径(画面px)
};

// 編集用の選択領域(ラバーバンド)の描画内容。App が画像座標→スクリーン座標へ変換済み。
struct SelectionView {
    bool visible = false;
    Point p1;               // 矩形の対角(順不同)
    Point p2;
    uint32_t borderRGB = 0;
    uint32_t fillARGB = 0;  // 上位バイト = アルファ
};

// 描画のプラットフォーム抽象。Windows 実装は Direct2D (renderer_d2d)。
// UI スレッドからのみ呼ばれる。
class IRenderer {
public:
    virtual ~IRenderer() = default;

    virtual void resize(uint32_t width, uint32_t height) = 0;

    // image は nullptr 可(背景のみ描画)。zoom は補間モード選択のヒント。
    virtual void render(const std::shared_ptr<const DecodedImage>& image,
                        const Matrix3x2& imageToScreen, float zoom, uint32_t backgroundRGB,
                        const AnnotationsView& annotations, const SelectionView& selection,
                        const SidebarView& sidebar, const StatusBarView& statusBar) = 0;
};

} // namespace blinker
