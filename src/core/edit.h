#pragma once

#include <memory>

#include "platform/decoder.h"

/**
 * @file edit.h
 * @brief 画像編集(トリミング・注釈の合成)の純関数。
 *
 * OS ヘッダに依存しない(単体テスト対象)。
 */

namespace blinker {

/**
 * @brief 画像座標の整数矩形。
 * @note w/h は正であること(正規化済みを前提とする)。
 */
struct RectI {
    int x = 0;  ///< 左端の X 座標
    int y = 0;  ///< 上端の Y 座標
    int w = 0;  ///< 幅
    int h = 0;  ///< 高さ
};

/**
 * @brief 矩形で切り出したコピーを返す。
 * @param[in] src  切り出し元の画像。
 * @param[in] rect 切り出す矩形。画像内へクランプされる。
 * @return 切り出された画像。有効領域が残らなければ nullptr。
 */
std::shared_ptr<DecodedImage> cropImage(const DecodedImage& src, RectI rect);

/**
 * @brief オーバーレイ画像を over 合成する。
 * @param[in,out] dst     合成先の画像(32bpp PBGRA)。破壊的に更新する。
 * @param[in]     overlay 重ねる画像(32bpp PBGRA、事前乗算)。
 * @param[in]     x       合成先での左端 X 座標。
 * @param[in]     y       合成先での上端 Y 座標。はみ出した分はクリップする。
 */
void blendOverlay(DecodedImage& dst, const DecodedImage& overlay, int x, int y);

} // namespace blinker
