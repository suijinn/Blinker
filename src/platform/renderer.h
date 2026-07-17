#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/geometry.h"
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

// 描画のプラットフォーム抽象。Windows 実装は Direct2D (renderer_d2d)。
// UI スレッドからのみ呼ばれる。
class IRenderer {
public:
    virtual ~IRenderer() = default;

    virtual void resize(uint32_t width, uint32_t height) = 0;

    // image は nullptr 可(背景のみ描画)。zoom は補間モード選択のヒント。
    virtual void render(const std::shared_ptr<const DecodedImage>& image,
                        const Matrix3x2& imageToScreen, float zoom, uint32_t backgroundRGB,
                        const SidebarView& sidebar, const StatusBarView& statusBar) = 0;
};

} // namespace blinker
