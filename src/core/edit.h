#pragma once

#include <cstdint>
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

/**
 * @brief 半透明を単色の背景へ焼き込んだ不透明なコピーを返す。
 *
 * 透明部分を持つ画像を、アルファを見ない相手(OCR エンジンなど)へ渡すために使う。
 * 事前乗算のまま背景を足すだけなので、逆乗算による丸め誤差は入らない。
 *
 * @param[in] src           変換元の画像(32bpp PBGRA)。
 * @param[in] backgroundRGB 敷く背景色(0xRRGGBB)。
 * @return アルファがすべて 255 になったコピー。src が空なら nullptr。
 * @note 完全に透明な画素は元の色が残っていないため、背景色そのものになる
 *       (背景と同系色の文字は読めなくなる)。
 */
std::shared_ptr<DecodedImage> flattenOnBackground(const DecodedImage& src,
                                                  uint32_t backgroundRGB);

/**
 * @brief 半透明の画素を含むかを調べる。
 * @param[in] src 調べる画像(32bpp PBGRA)。
 * @return アルファが 255 未満の画素が 1 つでもあれば true。
 */
bool hasTransparency(const DecodedImage& src);

} // namespace blinker
