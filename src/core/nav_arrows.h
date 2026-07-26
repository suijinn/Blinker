#pragma once

#include <optional>

#include "core/geometry.h"

/**
 * @file nav_arrows.h
 * @brief ビューポートの左右に出す画像遷移用オーバーレイ矢印の幾何(純粋関数)。
 *
 * マウスだけで前後の画像へ移れることを見つけられるようにするための表示。
 * ポインタが左右の端の帯へ入ると現れ、ボタンの上でだけクリックが効く
 * (帯全体を当たりにすると、端をクリックしたつもりで画像が変わってしまう)。
 *
 * @note **使用感が合わなければ廃止できるよう、判定と寸法をここに閉じ込めてある。**
 *       廃止するときに消すのは、このファイルと `NavArrowsView`
 *       (platform/renderer.h)、`App` の `navArrows` / `navArrowsGeometry` /
 *       `hitTestNavArrows` / `updateNavArrowHover`、各レンダラの `drawNavArrows`、
 *       `[view] nav_arrows` の 6 か所だけ。既存の入力・描画の流れには
 *       手を入れていない。
 */

namespace blinker {

constexpr float kNavArrowSizePx = 44.0f;    ///< ボタン(正方形)の一辺(画面px)
constexpr float kNavArrowMarginPx = 12.0f;  ///< ビューポート端からボタンまでの距離(画面px)
/// ポインタがビューポート端からこの距離に入るとボタンが現れる(画面px)
constexpr float kNavArrowBandPx = 110.0f;

/// @brief オーバーレイ矢印 1 つ分の状態。
struct NavArrow {
    bool visible = false;  ///< 表示するか
    bool hovered = false;  ///< ボタンの上にポインタがあるか(濃く描く)
    Point p1;              ///< ボタン領域の左上
    Point p2;              ///< ボタン領域の右下
};

/// @brief 左右のオーバーレイ矢印の状態。
struct NavArrowsState {
    NavArrow prev;  ///< 左(前の画像へ)
    NavArrow next;  ///< 右(次の画像へ)
};

/**
 * @brief オーバーレイ矢印の表示状態と領域を求める。
 *
 * 座標はビューポート左上を原点とする画面px(呼び出し側でスクリーン座標へ寄せる)。
 *
 * @param[in] viewport ビューポートの大きさ(サイドバー・ステータスバーを除く)。
 * @param[in] pointer  ポインタ位置。ウィンドウ外・ドラッグ中などで無効なら std::nullopt。
 * @param[in] hasPrev  前の画像があるか(先頭なら false)。
 * @param[in] hasNext  次の画像があるか(末尾なら false)。
 * @return 左右の矢印の状態。出さない場合は visible が false。
 * @note ボタンが収まらない狭いビューポートでは両方とも出さない。
 */
NavArrowsState navArrowsState(SizeF viewport, std::optional<Point> pointer, bool hasPrev,
                              bool hasNext);

/**
 * @brief ポインタ位置がどちらのボタンに当たるかを判定する。
 * @param[in] state   navArrowsState の結果(同じ座標系のもの)。
 * @param[in] pointer 判定するポインタ位置。
 * @return 次の画像のボタンなら true、前の画像なら false。どちらにも当たらなければ std::nullopt。
 * @note visible が false のボタンには当たらない。
 */
std::optional<bool> hitTestNavArrows(const NavArrowsState& state, Point pointer);

} // namespace blinker
