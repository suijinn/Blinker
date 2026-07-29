#pragma once

#include "core/geometry.h"

/**
 * @file pointer_state.h
 * @brief マウス操作の進行状態(ボタンの役割・パン・メニュー押下・ホイールの貯金)。
 *
 * OS ヘッダに依存しない(単体テスト対象)。App から切り出した状態で、画像も
 * ウィンドウも知らない ―「今どのボタンで何を始めているか」と、押下位置・回転量から
 * 決まる判定だけを持つ。掴んだ先(パンするのか注釈を動かすのか)を決めるのは App に残る。
 *
 * @note ここが持つのはポインタ由来の状態だけで、注釈の編集ドラッグ (EditDragState) と
 *       オブジェクト操作 (ObjectDragState) は別に持つ(画像の内容に触るため)。
 */

namespace blinker {

/**
 * @brief 物理マウスボタン。
 *
 * どちらのボタンが何をするかは blinker.ini の `[mouse] swap_buttons` で
 * 入れ替えられる(PointerState::role)。メニューとオブジェクトを掴む操作だけは
 * 入れ替えの対象外で、メニューは常に右ボタンのクリック、オブジェクトの選択・
 * 移動・回転・サイズ変更は常に左ボタンで行う。
 */
enum class MouseButton {
    Left,   ///< 左ボタン
    Right,  ///< 右ボタン
};

/**
 * @brief マウスボタンに割り当てられた役割。
 *
 * 既定は左が Pan・右が Edit。`[mouse] swap_buttons = true` で入れ替わる。
 * 注釈オブジェクトを掴む操作はどちらの役割にも属さない(常に左ボタン)。
 */
enum class MouseRole {
    Pan,   ///< 何も掴まなかったドラッグで画像をパンする
    Edit,  ///< 何も掴まなかったドラッグで現在のツール (EditTool) を実行する
};

/**
 * @brief 右ボタンを離したときに開くメニュー。
 *
 * メニューは押した時点では開かない。ドラッグにならずに離されたときだけ、
 * 押した場所に応じたメニューを開く。
 */
enum class MenuOnRelease {
    None,     ///< 開かない(押していない、またはドラッグとみなす距離を動いた)
    Pointer,  ///< ポインタ位置のメニュー(ツール切り替え・オブジェクト)
    Sidebar,  ///< サイドバーの一覧メニュー(並び替え・サブフォルダ)
};

/**
 * @brief マウス操作の進行状態。
 *
 * @note UI スレッド専用(App と同じ)。スレッド安全ではない。
 */
class PointerState {
public:
    /// これ未満の移動はクリックとみなす(画面px)。メニューを開くかの判定に使い、
    /// 編集ドラッグ (App::endEditDrag) も同じ値で「動いていない」を判定する
    static constexpr float kDragThresholdPx = 4.0f;

    /**
     * @brief 指定したボタンの役割を返す。
     * @param[in] button 対象のボタン。
     * @return 役割。既定では左が MouseRole::Pan・右が MouseRole::Edit で、
     *         入れ替え設定が有効なら逆になる。
     */
    MouseRole role(MouseButton button) const;

    /// @brief 左右の役割を入れ替えているか。
    /// @return 入れ替えていれば true。
    bool swapButtons() const { return swapButtons_; }

    /**
     * @brief 左右の役割の入れ替えを設定する(`[mouse] swap_buttons`)。
     * @param[in] swap 入れ替えるなら true。
     * @note メニューは対象外(常に右ボタン)。
     */
    void setSwapButtons(bool swap) { swapButtons_ = swap; }

    /// @brief 最後に観測したポインタ位置を返す。
    /// @return スクリーン座標。
    Point lastScreen() const { return lastScreen_; }

    /**
     * @brief ポインタの移動を記録する。
     * @param[in] screenPos 新しい位置(スクリーン座標)。
     * @return 直前の位置からの移動量(パンの差分に使う)。
     * @note ウィンドウ内にいる印も付ける(inside() が true になる)。
     */
    Point moveTo(Point screenPos);

    /**
     * @brief ポインタ位置だけを記録する。
     * @param[in] screenPos 新しい位置(スクリーン座標)。
     * @note ボタンを離した位置の記録用。ドラッグはウィンドウの外で終わりうるので、
     *       moveTo と違って inside() は変えない。
     */
    void setLastScreen(Point screenPos) { lastScreen_ = screenPos; }

    /// @brief ポインタがウィンドウ内にあるか(オーバーレイ矢印の表示判定)。
    /// @return 内側なら true。
    bool inside() const { return inside_; }

    /**
     * @brief ポインタがウィンドウ内にあるかを設定する。
     * @param[in] inside 内側なら true。
     */
    void setInside(bool inside) { inside_ = inside; }

    /// @brief パン中か(パン役のボタンで何も掴まなかったドラッグ)。
    /// @return パン中なら true。
    bool panning() const { return panning_; }

    /// @brief パンを開始する。
    void beginPan() { panning_ = true; }

    /// @brief パンを終える。
    void endPan() { panning_ = false; }

    /**
     * @brief 右ボタンの押下を記録する(メニューを開く候補にする)。
     * @param[in] screenPos 押下位置(スクリーン座標)。移動量の判定の基準になる。
     * @param[in] onSidebar 押下位置がサイドバー上か(一覧のメニューを開く)。
     */
    void pressMenu(Point screenPos, bool onSidebar);

    /// @brief メニューを開く候補として押されているか。
    /// @return 押されていれば true。
    bool menuPressed() const { return menuPressed_; }

    /// @brief メニューを開く候補から外す(次の押下の開始時と、別の操作が押下を消費したとき)。
    void cancelMenu();

    /**
     * @brief 右ボタンの解放を処理し、開くべきメニューを返す。
     * @param[in] screenPos 解放位置(スクリーン座標)。
     * @return 開くメニュー。押していない、または押下位置から kDragThresholdPx 以上
     *         動いていれば MenuOnRelease::None。
     * @note 押下状態は必ず消費する(何を返しても押されていない状態へ戻る)。
     */
    MenuOnRelease releaseMenu(Point screenPos);

    /// @brief 水平ホイールを 1 段と数えるまでのノッチ数を返す。
    /// @return ノッチ数。既定は垂直と同じ 1.0。
    float horizontalThreshold() const { return horizontalThreshold_; }

    /**
     * @brief 水平ホイールを 1 段と数えるまでのノッチ数を設定する。
     * @param[in] notches ノッチ数(`[mouse] wheel_horizontal_threshold`)。
     * @note 大きいほど鈍くなる。縦スクロールに横成分が混ざる入力装置での誤爆対策。
     */
    void setHorizontalThreshold(float notches) { horizontalThreshold_ = notches; }

    /**
     * @brief ホイールの回転量を貯金し、確定した段数を返す。
     * @param[in] notches    今回の回転量(ノッチ単位)。
     * @param[in] horizontal 水平ホイールなら true(貯金も 1 段の重みも垂直とは別)。
     * @return 確定した段数(0 以上)。向きはコマンド側が決めるので絶対値を返す。
     */
    int wheelSteps(float notches, bool horizontal);

    /// @brief 垂直ホイールの貯金を捨てる。
    void resetVerticalWheel() { wheelAccumV_ = 0.0f; }

    /// @brief 水平ホイールの貯金を捨てる。
    void resetHorizontalWheel() { wheelAccumH_ = 0.0f; }

private:
    bool swapButtons_ = false;   ///< 左右の役割を入れ替えているか
    bool panning_ = false;       ///< パン中か
    Point lastScreen_{};         ///< 最後に観測したポインタ位置
    bool inside_ = false;        ///< ポインタがウィンドウ内にあるか
    bool menuPressed_ = false;   ///< 右ボタンをメニューを開ける場所で押したか
    bool menuOnSidebar_ = false; ///< 上記の押下位置がサイドバーだったか
    Point menuPressScreen_{};    ///< 上記の押下位置(移動量の判定用)
    /// ホイールでコマンドを実行するときの回転量の貯金(垂直 / 水平で別に持つ)。
    /// 1 ノッチ未満ずつ通知される高精細ホイールでも取りこぼさないため
    float wheelAccumV_ = 0.0f;
    float wheelAccumH_ = 0.0f;         ///< 同・水平ホイール分
    float horizontalThreshold_ = 1.0f; ///< 水平ホイールを 1 段と数えるノッチ数
};

}  // namespace blinker
