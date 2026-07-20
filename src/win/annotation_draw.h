#pragma once

#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include "platform/annotation.h"

/**
 * @file annotation_draw.h
 * @brief 注釈図形の D2D 描画コード。
 *
 * ラスタライザ(保存時の焼き込み)とレンダラ(ライブ表示)で共用し、
 * 両者の見た目を一致させる。
 */

namespace blinker {

/**
 * @brief 矢印ヘッド(塗りつぶし三角形)の長さを求める。
 * @param[in] strokeWidth 線幅。長さはこれに比例させる。
 * @return 矢印ヘッドの長さ(strokeWidth と同じ座標系)。
 */
float arrowHeadLength(float strokeWidth);

/**
 * @brief Text 注釈のレイアウトを作る。
 *
 * p1/p2 の幅で折り返す。幅が小さすぎるときは 1 文字分を確保する。
 *
 * @param[in] dwrite    DirectWrite ファクトリ。
 * @param[in] spec      レイアウトする Text 注釈。
 * @param[in] maxHeight レイアウトの最大高さ(画像座標)。
 * @return 生成されたレイアウト。失敗時は nullptr。
 */
Microsoft::WRL::ComPtr<IDWriteTextLayout> createAnnotationTextLayout(
    IDWriteFactory* dwrite, const AnnotationSpec& spec, float maxHeight);

/**
 * @brief 注釈を画像座標のまま描画先へ描く。
 * @param[in,out] target  描画先のレンダーターゲット。
 * @param[in]     factory D2D ファクトリ(ジオメトリ生成に使う)。
 * @param[in]     dwrite  DirectWrite ファクトリ(Text 注釈に使う)。
 * @param[in]     spec    描画する注釈。
 * @param[in]     brush   描画に使うブラシ。spec の色が設定済みであること。
 * @note 回転や画像 → スクリーン変換は呼び出し側が SetTransform で設定しておくこと。
 */
void drawAnnotationShape(ID2D1RenderTarget* target, ID2D1Factory* factory,
                         IDWriteFactory* dwrite, const AnnotationSpec& spec,
                         ID2D1SolidColorBrush* brush);

} // namespace blinker
