#pragma once

#include "core/annotation_edit.h"
#include "core/geometry.h"
#include "platform/annotation.h"

/**
 * @file object_drag_state.h
 * @brief 注釈オブジェクトに対する進行中のドラッグ(移動・回転・サイズ変更)の状態。
 *
 * OS ヘッダに依存しない(単体テスト対象)。**画像も窓も知らず、注釈の一覧も持たない** ―
 * 対象は App の選択(`selected_`)が指し、ここは開始時点の写しと基準値だけを持つ。
 * ハンドルのヒット判定・座標変換・実際の変形(resizeAnnotation)は App に残る。
 */

namespace blinker {

/**
 * @brief 注釈オブジェクトに対する進行中のドラッグ操作。
 *
 * どれも常に左ボタンで行う(`[mouse] swap_buttons` の対象外)。
 */
enum class ObjectDragMode {
    None,    ///< 掴んでいない
    Move,    ///< 本体を掴んで移動中
    Rotate,  ///< 回転ハンドルを掴んで回転中
    Resize,  ///< サイズ変更ハンドルを掴んで拡縮中
};

/**
 * @brief 注釈オブジェクトを掴んでいる間の状態。
 *
 * 変形はドラッグ開始時の注釈(origSpec)を基準に毎回計算し直す。途中経過に対して
 * 差分を積むと、行きつ戻りつするドラッグで誤差が溜まるため。
 *
 * @note UI スレッド専用(App と同じ)。スレッド安全ではない。
 */
class ObjectDragState {
public:
    /**
     * @brief 移動を開始する。
     * @param[in] imagePos 掴んだ位置(画像座標)。移動量の基準になる。
     * @param[in] spec     掴んだ時点の注釈(移動の基準)。
     */
    void beginMove(Point imagePos, const AnnotationSpec& spec);

    /**
     * @brief 回転を開始する。
     * @param[in] spec             掴んだ時点の注釈(角度の基準)。
     * @param[in] pointerAngleDeg  中心から見たポインタの角度(度)。回転量の基準になる。
     */
    void beginRotate(const AnnotationSpec& spec, float pointerAngleDeg);

    /**
     * @brief サイズ変更を開始する。
     * @param[in] spec   掴んだ時点の注釈(拡縮の基準)。
     * @param[in] handle 掴んだハンドル。
     */
    void beginResize(const AnnotationSpec& spec, ResizeHandle handle);

    /// @brief 掴んでいたものを離す(どの操作でも共通)。
    void end() { mode_ = ObjectDragMode::None; }

    /// @brief 進行中の操作を返す。
    /// @return 掴んでいなければ ObjectDragMode::None。
    ObjectDragMode mode() const { return mode_; }

    /// @brief 何かを掴んでいるかを返す。
    /// @return 掴んでいれば true。
    bool active() const { return mode_ != ObjectDragMode::None; }

    /**
     * @brief ドラッグ開始時の注釈を返す。
     * @return 掴んだ時点の写し。移動・回転・サイズ変更のすべての基準になる。
     */
    const AnnotationSpec& origSpec() const { return origSpec_; }

    /**
     * @brief 掴んだハンドルを返す。
     * @return サイズ変更で掴んだハンドル(ObjectDragMode::Resize のときだけ意味を持つ)。
     */
    ResizeHandle handle() const { return handle_; }

    /**
     * @brief 掴んだ位置からの移動量を返す。
     * @param[in] imagePos 現在位置(画像座標)。
     * @return 掴んだ位置からの差分。開始時の注釈をこれだけ平行移動する。
     */
    Point moveDelta(Point imagePos) const {
        return {imagePos.x - startImage_.x, imagePos.y - startImage_.y};
    }

    /**
     * @brief ポインタの角度から、注釈に与える角度を求める。
     * @param[in] pointerAngleDeg 中心から見たポインタの角度(度)。
     * @return 開始時の角度に、掴んでからのポインタの回転量を足したもの(度)。
     * @note スナップと正規化は呼び出し側で行う(Shift の有無を知らないため)。
     */
    float rotatedAngleDeg(float pointerAngleDeg) const {
        return origSpec_.angleDeg + pointerAngleDeg - startAngleDeg_;
    }

private:
    ObjectDragMode mode_ = ObjectDragMode::None;  ///< 進行中の操作
    Point startImage_{};                          ///< Move: 掴んだ画像座標
    AnnotationSpec origSpec_;                     ///< 開始時の注釈(変形の基準)
    float startAngleDeg_ = 0;                     ///< Rotate: 開始時のポインタ角度
    ResizeHandle handle_ = ResizeHandle::BottomRight;  ///< Resize: 掴んだハンドル
};

}  // namespace blinker
