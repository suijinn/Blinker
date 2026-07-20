#pragma once

#include <array>
#include <optional>
#include <vector>

#include "core/geometry.h"
#include "platform/annotation.h"

/**
 * @file annotation_edit.h
 * @brief 注釈オブジェクトの選択・編集(ヒットテスト・幾何)の純関数。
 *
 * OS ヘッダに依存しない(単体テスト対象)。座標はすべて画像座標
 * (rotationHandlePos のみスクリーン座標を返す)。
 */

namespace blinker {

/// @brief 注釈の回転前バウンディングボックス(p1/p2 を正規化したもの)。
struct BoundsF {
    float minX = 0;  ///< 左端の X 座標
    float minY = 0;  ///< 上端の Y 座標
    float maxX = 0;  ///< 右端の X 座標
    float maxY = 0;  ///< 下端の Y 座標
};

/**
 * @brief 点を中心周りに回転する。
 * @param[in] p      回転する点。
 * @param[in] center 回転の中心。
 * @param[in] deg    回転角(度)。スクリーン座標系 = Y 下向きで時計回り
 *                   (D2D の Matrix3x2F::Rotation と同じ向き)。
 * @return 回転後の点。
 */
Point rotateAround(Point p, Point center, float deg);

/**
 * @brief 注釈の回転前バウンディングボックスを求める。
 * @param[in] spec 対象の注釈。
 * @return p1/p2 を正規化した矩形(画像座標)。
 */
BoundsF annotationBounds(const AnnotationSpec& spec);

/**
 * @brief 注釈のバウンディングボックス中心を求める。
 * @param[in] spec 対象の注釈。
 * @return 中心点(画像座標)。回転の中心でもある。
 */
Point annotationCenter(const AnnotationSpec& spec);

/**
 * @brief 回転後の四隅を求める(選択枠の描画に使う)。
 * @param[in] spec 対象の注釈。angleDeg で回転させる。
 * @return 左上・右上・右下・左下の順に並んだ 4 点(画像座標)。
 */
std::array<Point, 4> rotatedCorners(const AnnotationSpec& spec);

/**
 * @brief 点が注釈に当たるかを判定する。
 *
 * Rect/Ellipse は輪郭線の近傍のみ(内部は空けてパン操作を妨げない)、
 * Line/Arrow は線分への距離、Text はバウンディングボックス内部で判定する。
 *
 * @param[in] spec      対象の注釈。
 * @param[in] imagePos  判定する点(画像座標)。
 * @param[in] tolerance 許容距離(画像座標)。
 * @return 当たっていれば true。
 */
bool hitTestAnnotation(const AnnotationSpec& spec, Point imagePos, float tolerance);

/**
 * @brief 注釈一覧に対してヒットテストする。
 * @param[in] specs     判定対象の注釈一覧。末尾ほど手前に描かれる。
 * @param[in] imagePos  判定する点(画像座標)。
 * @param[in] tolerance 許容距離(画像座標)。
 * @return 最前面(末尾)から検索して最初に当たった index。当たらなければ std::nullopt。
 */
std::optional<size_t> hitTestAnnotations(const std::vector<AnnotationSpec>& specs,
                                         Point imagePos, float tolerance);

/**
 * @brief 注釈を平行移動する。
 * @param[in,out] spec 移動する注釈。p1/p2 が破壊的に更新される。
 * @param[in]     dx   X 方向の移動量(画像座標)。
 * @param[in]     dy   Y 方向の移動量(画像座標)。
 */
void translateAnnotation(AnnotationSpec& spec, float dx, float dy);

/**
 * @brief サイズ変更ハンドルの種類。
 *
 * Rect/Ellipse は四隅 + 四辺、Text は四隅 + 左右(高さは内容から決まる)、
 * Line/Arrow は両端点(P1/P2)を使う。
 */
enum class ResizeHandle {
    TopLeft, Top, TopRight, Right, BottomRight, Bottom, BottomLeft, Left, P1, P2
};

/// @brief サイズ変更ハンドルの種類と位置の組。
struct ResizeHandlePos {
    ResizeHandle handle;  ///< ハンドルの種類
    Point pos;            ///< 画像座標(angleDeg 回転済み)
};

/**
 * @brief 注釈のサイズ変更ハンドルの位置を列挙する。
 * @param[in] spec 対象の注釈。
 * @return 種類の異なるハンドルの位置一覧(画像座標)。
 */
std::vector<ResizeHandlePos> resizeHandlePositions(const AnnotationSpec& spec);

/**
 * @brief ハンドルをドラッグした結果の注釈を返す。
 *
 * 回転中でもアンカー(反対側の角/辺中点、端点ドラッグでは他端)の見た目の位置を固定する。
 *
 * @param[in] orig       ドラッグ開始時の注釈。
 * @param[in] handle     掴んでいるハンドル。
 * @param[in] mouseImage 現在のポインタ位置(画像座標)。
 * @param[in] keepAspect true なら四隅ハンドルで縦横比を維持する(Shift ドラッグ用)。
 * @return サイズ変更後の注釈。
 */
AnnotationSpec resizeAnnotation(const AnnotationSpec& orig, ResizeHandle handle,
                                Point mouseImage, bool keepAspect);

/**
 * @brief 回転ハンドルの位置を求める。
 * @param[in] spec          対象の注釈。
 * @param[in] imageToScreen 画像座標 → スクリーン座標の変換行列。
 * @param[in] offsetPx      選択枠上辺の中点から外側へ離す距離(画面 px)。
 * @return 回転ハンドルの位置(スクリーン座標)。
 */
Point rotationHandlePos(const AnnotationSpec& spec, const Matrix3x2& imageToScreen,
                        float offsetPx);

/**
 * @brief 2 点間の角度を求める。
 * @param[in] center 基準となる中心点。
 * @param[in] p      角度を測る対象の点。
 * @return 角度(度)。スクリーン座標系 = Y 下向きで時計回り、右方向が 0°。
 */
float angleDegFrom(Point center, Point p);

/**
 * @brief 角度を最寄りの step 倍数へ丸める。
 * @param[in] deg  丸める角度(度)。
 * @param[in] step スナップの刻み(度)。
 * @return 丸められた角度(度)。
 */
float snapAngleDeg(float deg, float step);

/**
 * @brief 角度を [0, 360) の範囲へ正規化する。
 * @param[in] deg 正規化する角度(度)。負値・360 以上も受け付ける。
 * @return [0, 360) に収まる角度(度)。
 */
float normalizeAngleDeg(float deg);

} // namespace blinker
