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
 * spec.styles の太字・斜体・下線も適用する(いずれも文字送り・行高に影響するため、
 * キャレット位置やヒットテストの計測もこのレイアウトで行えば表示と一致する)。
 *
 * @param[in] dwrite    DirectWrite ファクトリ。
 * @param[in] spec      レイアウトする Text 注釈。
 * @param[in] maxHeight レイアウトの最大高さ(画像座標)。
 * @return 生成されたレイアウト。失敗時は nullptr。
 * @note 部分書式の色は含まれない(描画時に applyTextColorEffects で与える)。
 */
Microsoft::WRL::ComPtr<IDWriteTextLayout> createAnnotationTextLayout(
    IDWriteFactory* dwrite, const AnnotationSpec& spec, float maxHeight);

/**
 * @brief Text 注釈の箱(塗りつぶし・枠線を描く矩形)を求める。
 *
 * 左上はバウンディングボックスの左上、大きさはレイアウトの実測値。
 * ラスタライザが確保するオーバーレイ領域もこの矩形を基準にしているため、
 * ライブ表示と焼き込みで箱の位置・大きさが一致する。
 *
 * @param[in] spec   対象の Text 注釈。
 * @param[in] layout 実測に使うレイアウト。nullptr なら p1/p2 の矩形を返す。
 * @return 箱の矩形(画像座標、回転前)。
 */
D2D1_RECT_F textBoxRect(const AnnotationSpec& spec, IDWriteTextLayout* layout);

/**
 * @brief 部分書式の文字色を、レイアウトの drawing effect として設定する。
 *
 * D2D の既定テキストレンダラは drawing effect に設定された ID2D1Brush を
 * その範囲の描画に使う。色を指定していない範囲は DrawTextLayout に渡した
 * ブラシ(注釈全体の色)のまま描かれる。
 *
 * @param[in]     target ブラシの生成元となるレンダーターゲット。
 * @param[in,out] layout 効果を設定するレイアウト。破壊的に更新される。
 * @param[in]     spec   描画する Text 注釈。styles の色だけを見る。
 * @note ブラシはデバイス依存のため、描画に使う target と同じもので作ること。
 */
void applyTextColorEffects(ID2D1RenderTarget* target, IDWriteTextLayout* layout,
                           const AnnotationSpec& spec);

/**
 * @brief drawAnnotationShape で描く部分の指定。
 *
 * インプレース編集中の選択範囲ハイライトは文字の下・塗りつぶしの上に敷く必要があるため、
 * 呼び出し側が背景と前景を分けて描けるようにしている。
 */
enum class AnnotationDrawParts {
    All,         ///< 背景と前景の両方(通常はこれ)
    Background,  ///< 塗りつぶしと Text の枠線だけ
    Foreground,  ///< 輪郭線・矢印・文字だけ
};

/**
 * @brief 注釈を画像座標のまま描画先へ描く。
 * @param[in,out] target  描画先のレンダーターゲット。
 * @param[in]     factory D2D ファクトリ(ジオメトリ生成に使う)。
 * @param[in]     dwrite  DirectWrite ファクトリ(Text 注釈に使う)。
 * @param[in]     spec    描画する注釈。
 * @param[in]     brush   輪郭線・文字に使うブラシ。spec.colorRGB が設定済みであること。
 *                        背景のみを描く場合(parts = Background)は使わない。
 * @param[in]     parts   描く部分。既定は背景と前景の両方。
 * @note 回転や画像 → スクリーン変換は呼び出し側が SetTransform で設定しておくこと。
 * @note 塗りつぶし(fillRGB/fillAlpha)と Text の枠線(borderRGB/borderWidth)のブラシは
 *       半透明を扱うため target から内部で生成する。brush には影響しない。
 */
void drawAnnotationShape(ID2D1RenderTarget* target, ID2D1Factory* factory,
                         IDWriteFactory* dwrite, const AnnotationSpec& spec,
                         ID2D1SolidColorBrush* brush,
                         AnnotationDrawParts parts = AnnotationDrawParts::All);

} // namespace blinker
