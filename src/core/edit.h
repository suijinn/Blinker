#pragma once

#include <memory>

#include "platform/decoder.h"

namespace blinker {

// 画像編集(トリミング・注釈の合成)の純関数。OS ヘッダに依存しない(単体テスト対象)。

// 画像座標の整数矩形。w/h は正であること(正規化済み)。
struct RectI {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

// rect を画像内にクランプして切り出したコピーを返す。有効領域が残らなければ nullptr。
std::shared_ptr<DecodedImage> cropImage(const DecodedImage& src, RectI rect);

// overlay (32bpp PBGRA 事前乗算) を dst の (x, y) へ over 合成する。はみ出しはクリップ。
void blendOverlay(DecodedImage& dst, const DecodedImage& overlay, int x, int y);

} // namespace blinker
