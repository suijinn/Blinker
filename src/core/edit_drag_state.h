#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include "core/geometry.h"

/**
 * @file edit_drag_state.h
 * @brief 編集ドラッグ(何も掴まなかったドラッグで図形・範囲を作る操作)の進行状態。
 *
 * OS ヘッダに依存しない(単体テスト対象)。**画像もツールも窓も知らない** ―
 * 押下位置と現在位置、手書きの軌跡だけを持つ。画像座標への変換・クランプ・
 * Shift での方向合わせ・どのツールを適用するかは App に残る。
 */

namespace blinker {

/**
 * @brief 編集ドラッグの進行状態。
 *
 * 始点は押下時に決まって動かず、終点だけが追従する。手書き(ペン・マーカー)は
 * 始点と終点ではなく通過点の列そのものが図形になるため、その軌跡もここが持つ。
 *
 * @note UI スレッド専用(App と同じ)。スレッド安全ではない。
 */
class EditDragState {
public:
    /**
     * @brief ドラッグを開始する。
     * @param[in] screenPos 押下位置(スクリーン座標)。移動量の判定の基準になる。
     * @param[in] imagePos  押下位置(画像座標)。呼び出し側で画像内へクランプしておく。
     * @param[in] pen       手書きツールなら true(軌跡の 1 点目に始点を置く)。
     * @note 終点は始点と同じ位置から始まる。前回の軌跡と直線アンカーは捨てられる。
     */
    void begin(Point screenPos, Point imagePos, bool pen);

    /**
     * @brief ドラッグを終える。
     * @note **軌跡は残す。** 終了後に確定処理が図形へ移すため。次の begin() で捨てられる。
     */
    void end() { dragging_ = false; }

    /// @brief ドラッグも軌跡も捨てる(編集の破棄)。
    void reset();

    /// @brief ドラッグ中かを返す。
    /// @return ドラッグ中なら true。
    bool dragging() const { return dragging_; }

    /// @brief 始点を返す。
    /// @return 押下位置(画像座標)。ズームしても動かない。
    Point startImage() const { return startImage_; }

    /// @brief 終点を返す。
    /// @return 現在位置(画像座標)。
    Point endImage() const { return endImage_; }

    /**
     * @brief 終点を設定する。
     * @param[in] imagePos 現在位置(画像座標)。クランプと Shift の方向合わせは呼び出し側。
     */
    void setEndImage(Point imagePos) { endImage_ = imagePos; }

    /**
     * @brief 押下位置から十分に動いたかを返す。
     * @param[in] screenPos   現在位置(スクリーン座標)。
     * @param[in] thresholdPx クリックとみなす移動量(画面px)。
     * @return 押下位置から thresholdPx 以上動いていれば true。
     * @note false なら単なるクリックなので、確定しても何も作らない。
     */
    bool movedEnough(Point screenPos, float thresholdPx) const;

    /// @brief 手書きの軌跡を返す。
    /// @return 通過点の列(画像座標)。手書き以外では空。
    const std::vector<Point>& penPoints() const { return penPoints_; }

    /**
     * @brief 直線アンカーを今の軌跡の末尾に置く(Shift を押し始めた)。
     * @note 既にアンカーがあるなら動かさない。押している間ずっと同じ点を起点にするため。
     *       軌跡が空なら何もしない。
     */
    void anchorStraight();

    /// @brief 直線アンカーを外す(Shift を離した、手書き以外へ切り替わった)。
    void resetStraightAnchor() { straightAnchor_.reset(); }

    /**
     * @brief 直線アンカーが指す点を返す。
     * @return アンカーの位置(画像座標)。アンカーが無ければ std::nullopt。
     * @note Shift 中の直線は、この点から終点までの 1 本になる。
     */
    std::optional<Point> straightAnchorPoint() const;

    /**
     * @brief 手書きの軌跡を終点まで伸ばす。
     * @param[in] minDistancePx 直前の点との最小距離(画像座標)。これ未満の動きは捨てる。
     * @note 直線アンカーがある間は、アンカーから先を捨ててまっすぐな 1 本で引き直す
     *       (押す前に描いた軌跡は残る)。
     */
    void extendPen(float minDistancePx);

private:
    bool dragging_ = false;                  ///< ドラッグ中か
    Point startScreen_{};                    ///< 押下位置(移動量の判定用)
    Point startImage_{};                     ///< 始点(画像座標。ズーム中も不変)
    Point endImage_{};                       ///< 終点(画像座標。ズーム中も不変)
    std::vector<Point> penPoints_;           ///< 手書きの軌跡(画像座標)
    /// Shift を押し始めたときの penPoints_ の末尾 index。押している間、ここから先は
    /// まっすぐな 1 本の線で置き換える(押す前に描いた軌跡は残る)。離すと無効になる
    std::optional<size_t> straightAnchor_;
};

}  // namespace blinker
