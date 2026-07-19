#pragma once

#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include "platform/annotation.h"

namespace blinker {

// 注釈図形の D2D 描画コード。ラスタライザ(保存時の焼き込み)とレンダラ(ライブ表示)で
// 共用し、両者の見た目を一致させる。

// 矢印ヘッド(塗りつぶし三角形)の長さ。線幅に比例させる
float arrowHeadLength(float strokeWidth);

// Text 注釈のレイアウトを作る(p1/p2 の幅で折り返し。幅が小さすぎるときは1文字分を確保)。
// 失敗時 nullptr
Microsoft::WRL::ComPtr<IDWriteTextLayout> createAnnotationTextLayout(
    IDWriteFactory* dwrite, const AnnotationSpec& spec, float maxHeight);

// spec を画像座標のまま target へ描く。回転や画像→スクリーン変換は呼び出し側が
// SetTransform で設定しておくこと。brush には spec の色が設定済みであること
void drawAnnotationShape(ID2D1RenderTarget* target, ID2D1Factory* factory,
                         IDWriteFactory* dwrite, const AnnotationSpec& spec,
                         ID2D1SolidColorBrush* brush);

} // namespace blinker
