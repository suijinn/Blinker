#pragma once

#include <array>
#include <optional>
#include <vector>

#include "core/geometry.h"
#include "platform/annotation.h"

namespace blinker {

// 注釈オブジェクトの選択・編集(ヒットテスト・幾何)の純関数。OS ヘッダに依存しない
// (単体テスト対象)。座標はすべて画像座標(rotationHandlePos のみスクリーン座標を返す)。

// 注釈の回転前バウンディングボックス(p1/p2 の正規化)
struct BoundsF {
    float minX = 0;
    float minY = 0;
    float maxX = 0;
    float maxY = 0;
};

BoundsF annotationBounds(const AnnotationSpec& spec);
Point annotationCenter(const AnnotationSpec& spec);

// angleDeg 回転後の四隅(左上・右上・右下・左下の順)。選択枠の描画に使う
std::array<Point, 4> rotatedCorners(const AnnotationSpec& spec);

// 点が注釈に当たるか。Rect/Ellipse は輪郭線の近傍のみ(内部は空けてパン操作を妨げない)、
// Line/Arrow は線分への距離、Text はバウンディングボックス内部。tolerance は画像座標
bool hitTestAnnotation(const AnnotationSpec& spec, Point imagePos, float tolerance);

// 最前面(末尾)から検索して最初に当たった index を返す
std::optional<size_t> hitTestAnnotations(const std::vector<AnnotationSpec>& specs,
                                         Point imagePos, float tolerance);

void translateAnnotation(AnnotationSpec& spec, float dx, float dy);

// 回転ハンドルの位置(スクリーン座標)。選択枠上辺の中点から外側へ offsetPx 離れた点
Point rotationHandlePos(const AnnotationSpec& spec, const Matrix3x2& imageToScreen,
                        float offsetPx);

// center から p への角度(度、スクリーン座標系=Y下向きで時計回り、右方向が 0°)
float angleDegFrom(Point center, Point p);

float snapAngleDeg(float deg, float step);   // 最寄りの step 倍数へ丸める
float normalizeAngleDeg(float deg);          // [0, 360) へ正規化

} // namespace blinker
