#pragma once

#include <cstddef>

/**
 * @file sidebar_state.h
 * @brief サイドバー(ファイル名一覧・操作一覧)の表示状態。
 *
 * OS ヘッダに依存しない(単体テスト対象)。App から切り出した状態で、
 * 一覧の中身もウィンドウも知らない ―「開いているか・どのモードか・幅・スクロール量」と、
 * そこから決まる幾何だけを持つ。項目数や領域の高さのように外から決まる値は引数で受け取り、
 * フルスクリーンで隠す判断は App(`App::sidebarVisible`)に残っている。
 */

namespace blinker {

/**
 * @brief サイドバーに何を表示しているか。
 *
 * どちらのモードも同じ SidebarView(文字列のリスト)として描画される。
 * レンダラはモードを知らない。
 */
enum class SidebarMode {
    Files,  ///< フォルダ内のファイル名一覧(クリックでその画像へ移動)
    Help,   ///< 操作一覧(現在のキーバインドから生成。クリックは無効)
};

/**
 * @brief サイドバーの可視・モード・幅・スクロール量と、右端を掴む幅変更ドラッグの状態。
 *
 * 幅には 2 つある。設定 (ini) から来る `configuredWidth` と、モードに応じて広げた
 * 表示上の `width` で、外へ見せる幅・掴める右端の位置は常に後者。
 *
 * @note UI スレッド専用(App と同じ)。スレッド安全ではない。
 */
class SidebarState {
public:
    static constexpr float kItemHeight = 24.0f;  ///< 1 項目の高さ(px)
    /// 操作一覧モードでの最低幅。「操作名 + キー」が収まらないと用をなさないため広げる
    static constexpr float kHelpWidth = 300.0f;
    static constexpr float kMinWidth = 120.0f;     ///< 幅の下限(px)
    static constexpr float kMaxWidth = 480.0f;     ///< 幅の上限(px)
    /// 右端を掴める帯の幅(境界の左右へこの分だけ広がる)
    static constexpr float kResizeGripPx = 4.0f;
    /// 幅の変更で画像の表示領域をここまでは残す(狭いウィンドウでの上限)
    static constexpr float kMinViewportWidth = 120.0f;

    /// @brief サイドバーが開いているか(フルスクリーンでの非表示は含まない)。
    /// @return 開いていれば true。
    bool enabled() const { return enabled_; }

    /// @brief 表示モードを返す。
    /// @return ファイル名一覧か操作一覧か。閉じていても最後のモードを返す。
    SidebarMode mode() const { return mode_; }

    /**
     * @brief 指定モードで開いているかを返す。
     * @param[in] mode 判定するモード。
     * @return 開いていて、かつそのモードなら true。
     */
    bool showing(SidebarMode mode) const { return enabled_ && mode_ == mode; }

    /**
     * @brief 表示上の幅を返す。
     * @return 幅(px)。操作一覧モードでは kHelpWidth 以上に広げた値。
     * @note 開いているかは見ない(隠すかの判断は呼び出し側)。
     */
    float width() const;

    /// @brief 設定 (ini) とドラッグで決まる素の幅を返す。
    /// @return 幅(px)。モードによる下駄は履かせない。
    float configuredWidth() const { return width_; }

    /// @brief 幅の下限を返す。
    /// @return モードに応じた下限(px)。操作一覧モードでは kHelpWidth。
    float minWidth() const;

    /// @brief 一覧のスクロール量を返す。
    /// @return 先頭からの距離(px)。0 以上。
    float scroll() const { return scroll_; }

    /**
     * @brief 指定モードの表示を切り替える。
     *
     * 別のモードで開いている間は、閉じるのではなくそのモードへ切り替える。
     *
     * @param[in] mode 表示したいモード。
     * @return 切り替えた結果、開いた状態なら true。
     */
    bool toggle(SidebarMode mode);

    /**
     * @brief 開いているかを直接設定する(設定 (ini) の適用用)。
     * @param[in] enabled 開いた状態にするなら true。
     */
    void setEnabled(bool enabled) { enabled_ = enabled; }

    /**
     * @brief 設定 (ini) の幅を適用する。
     * @param[in] width 設定値(px)。kMinWidth 〜 kMaxWidth へクランプされる。
     * @note モードもウィンドウ幅も見ない(表示上の下限はモード側で効く)。
     */
    void setConfiguredWidth(float width);

    /**
     * @brief 幅を変更する。
     * @param[in] width 変更後の幅(px)。下限・上限へクランプされる。
     * @param[in] clientWidth クライアント領域の幅(px)。画像の表示領域を
     *                        kMinViewportWidth だけ残すため、上限がこれで決まる。
     * @return 実際に幅が変わったら true(ドラッグ中の無駄な再描画を避けるため)。
     */
    bool setWidth(float width, float clientWidth);

    /**
     * @brief スクロール量を直接設定する。
     * @param[in] scroll 先頭からの距離(px)。クランプはしない。
     */
    void setScroll(float scroll) { scroll_ = scroll; }

    /**
     * @brief 項目数ぶんスクロールする(ホイール操作)。
     * @param[in] items スクロールする項目数。負で先頭方向へ。
     * @note クランプはしない(呼び出し側で clampScroll を続けること)。
     */
    void scrollByItems(float items) { scroll_ += items * kItemHeight; }

    /**
     * @brief スクロール量を有効範囲へ収める。
     * @param[in] itemCount 一覧に並ぶ項目数。
     * @param[in] viewHeight サイドバー領域の高さ(px)。
     */
    void clampScroll(size_t itemCount, float viewHeight);

    /**
     * @brief 指定項目が見える位置までスクロールする。
     * @param[in] index 見えるようにしたい項目の位置。
     * @param[in] itemCount 一覧に並ぶ項目数。
     * @param[in] viewHeight サイドバー領域の高さ(px)。
     * @note 既に見えているなら動かさない。
     */
    void scrollToItem(size_t index, size_t itemCount, float viewHeight);

    /**
     * @brief サイドバー内の Y 座標にある項目の位置を返す。
     * @param[in] y サイドバー領域内の Y 座標(px)。
     * @return 項目の位置。項目数との比較は呼び出し側で行うこと。
     * @pre y >= 0(領域外の判定は呼び出し側で済ませてあること)。
     */
    size_t itemAt(float y) const;

    /// @brief 可視範囲の先頭にある項目の位置を返す。
    /// @return 項目の位置(上端が隠れていてもその項目を指す)。
    size_t firstVisibleItem() const;

    /// @brief 可視範囲の先頭項目を描き始める Y 座標を返す。
    /// @return サイドバー領域内の Y 座標(px)。上端が隠れていれば負。
    float firstItemY() const;

    /**
     * @brief 右端を掴む幅変更ドラッグを始める。
     * @param[in] screenX 掴んだ位置の X(スクリーン座標)。
     * @param[in] startWidth 開始時の幅(px)。見えている幅を渡して 1:1 で動かす。
     */
    void beginResize(float screenX, float startWidth);

    /// @brief 幅変更ドラッグ中か。
    /// @return ドラッグ中なら true。
    bool resizing() const { return resizing_; }

    /**
     * @brief ドラッグ中のポインタ位置から変更後の幅を求める。
     * @param[in] screenX 現在のポインタ位置の X(スクリーン座標)。
     * @return 掴んだ位置からの総移動量を足した幅(px)。クランプはしていない。
     * @note 直前の幅からの差分ではなく総移動量で決める(クランプで取りこぼしが出ないように)。
     */
    float resizeWidth(float screenX) const;

    /// @brief 幅変更ドラッグを終える。
    void endResize() { resizing_ = false; }

    /**
     * @brief 指定位置が右端を掴める帯の中かを返す。
     * @param[in] screenX 判定する位置の X(スクリーン座標)。
     * @return 掴める位置なら true。
     * @note 帯は境界をまたぐので、掴める位置はサイドバーの内側と外側の両方にある。
     *       開いているか・Y が領域内かは見ない(呼び出し側で判定すること)。
     */
    bool onResizeEdge(float screenX) const;

private:
    bool enabled_ = false;                   ///< 開いているか
    SidebarMode mode_ = SidebarMode::Files;  ///< 表示モード(閉じても保つ)
    float width_ = 220.0f;                   ///< 素の幅 (px)
    float scroll_ = 0.0f;                    ///< 一覧のスクロール量 (px)
    bool resizing_ = false;                  ///< 右端を掴んで幅を変更中か
    float resizeStartX_ = 0.0f;              ///< 変更開始時のポインタ X(スクリーン座標)
    float resizeStartWidth_ = 0.0f;          ///< 変更開始時の幅 (px)
};

}  // namespace blinker
