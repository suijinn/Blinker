#pragma once

#include <cstdint>
#include <memory>
#include <optional>

#include "core/geometry.h"
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
 * @brief 対角 2 点で表した範囲を、切り出す整数矩形へ丸めて画像内へ収める。
 *
 * 部分的にかかった画素も残すよう外側へ丸める(floor/ceil)。画像内へクランプ済みの
 * 矩形を返すので、原点 (x, y) はそのまま「切り出しで注釈をどれだけ平行移動するか」に
 * 使える ―― 画像の外まで広げた範囲でも、実際に切れる位置とずれない。
 *
 * @param[in] p1          範囲の対角の一方(画像座標)。
 * @param[in] p2          範囲の対角の他方(画像座標)。
 * @param[in] imageWidth  画像の幅(ピクセル)。
 * @param[in] imageHeight 画像の高さ(ピクセル)。
 * @return 切り出す矩形。画像と重ならなければ std::nullopt。
 * @note ここを通した矩形なら cropImage は必ず成功する(判定を二重に持たないため、
 *       メニューの表示可否と実行のどちらもこの関数で決める)。
 */
std::optional<RectI> cropRectFor(Point p1, Point p2, uint32_t imageWidth, uint32_t imageHeight);

/**
 * @brief 切り出す矩形を、指定の縦横比ちょうどになるよう整える。
 *
 * 画素数で比を厳密に保つため、大きさは比の**整数倍**へ丸める(16:9 なら 16k x 9k)。
 * 今の範囲へ収まる最大の整数倍を選び、中心を保ったまま置き直してから、はみ出す分だけ
 * 画像内へ寄せる(大きさは変えない)。丸めで最大 ratioW-1 px 縮むが、そのぶん
 * 「16:9」と表示したものが本当に 16:9 の画素数になる。
 *
 * @param[in] rect        今の範囲(cropRectFor が返したもの)。
 * @param[in] ratioW      縦横比の横。正であること。
 * @param[in] ratioH      縦横比の縦。正であること。
 * @param[in] imageWidth  画像の幅(ピクセル)。
 * @param[in] imageHeight 画像の高さ(ピクセル)。
 * @return 整えた矩形。比が不正、または今の範囲が比 1 倍ぶんにも満たなければ std::nullopt。
 * @pre rect が画像内に収まっていること(cropRectFor の戻り値ならば満たす)。
 * @note 既に指定の比ちょうどなら rect をそのまま返す(メニューのチェック判定に使える)。
 */
std::optional<RectI> fitRectToAspect(RectI rect, int ratioW, int ratioH, uint32_t imageWidth,
                                     uint32_t imageHeight);

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
