#pragma once

#include <SDL3/SDL.h>

#include <memory>
#include <optional>
#include <string>

#include "core/app.h"
#include "sdl/font_stb.h"
#include "sdl/renderer_sdl.h"

/**
 * @file window_sdl.h
 * @brief メインウィンドウ(SDL バックエンド)。
 */

namespace blinker {

/**
 * @brief メインウィンドウ(SDL バックエンド)。
 *
 * SDL イベントを App のコマンド/イベントに変換し、IAppHost を実装する。
 * 未対応の IAppHost API(コンテキストメニュー・テキスト入力・色選択)は nullopt を
 * 返し、編集機能は無効のまま閲覧機能をフルに提供する。
 *
 * @todo コンテキストメニュー・テキスト入力・色選択を実装し、編集機能を
 *       Windows 版と同等にする。SDL3 にはこれらの標準ダイアログがないため、
 *       自前描画のオーバーレイ UI が必要。
 */
class WindowSdl final : public IAppHost {
public:
    /**
     * @brief ウィンドウとレンダラを作成する。
     * @param[in] font UI テキスト描画に使うフォント。本オブジェクトより長生きすること。
     * @return 作成できたら true。SDL の初期化・ウィンドウ作成に失敗したら false。
     */
    bool create(FontStb& font);

    /**
     * @brief イベントの転送先となる App を結び付ける。
     * @param[in] app 転送先の App。本オブジェクトより長生きすること。
     */
    void attachApp(App* app);

    /// @brief イベントループを回す(quit されるまで戻らない)。
    void run();

    /**
     * @brief デコード完了をイベントとして投函する。
     * @note ImageCache のワーカースレッドから呼べる(SDL_PushEvent はスレッド安全)。
     */
    void postDecodedEvent();

    /// @name IAppHost の実装
    /// @{

    /// @brief 次のループ回で再描画するようフラグを立てる。
    void requestRedraw() override { needsRedraw_ = true; }

    /**
     * @brief ウィンドウタイトルを設定する。
     * @param[in] title 設定するタイトル(UTF-8)。
     */
    void setTitle(const std::string& title) override;

    /**
     * @brief フルスクリーン表示を切り替える。
     * @param[in] enabled true でフルスクリーン、false で通常ウィンドウへ復帰。
     */
    void setFullscreen(bool enabled) override;

    /**
     * @brief 現在フルスクリーンかを返す。
     * @return フルスクリーンなら true。
     */
    bool isFullscreen() const override { return fullscreen_; }

    /**
     * @brief ファイルを開くダイアログ (SDL_ShowOpenFileDialog) を表示する。
     * @return 選択されたパス。キャンセル時は std::nullopt。
     */
    std::optional<std::filesystem::path> showOpenDialog() override;

    /**
     * @brief 名前を付けて保存ダイアログ (SDL_ShowSaveFileDialog) を表示する。
     * @param[in] defaultFileName 初期表示するファイル名(UTF-8)。
     * @return 選択されたパス。キャンセル時は std::nullopt。
     */
    std::optional<std::filesystem::path> showSaveDialog(
        const std::string& defaultFileName) override;

    /**
     * @brief ポップアップメニューを表示する(SDL バックエンドでは未実装)。
     * @param[in] items     メニュー構造(未使用)。
     * @param[in] screenPos 表示位置(未使用)。
     * @return 常に std::nullopt。閲覧機能には影響しない。
     */
    std::optional<size_t> showContextMenu(const std::vector<MenuItem>& items,
                                          Point screenPos) override;

    /**
     * @brief テキスト編集の開始・終了を受け取る(SDL バックエンドでは未実装)。
     * @param[in] active         編集中かどうか(未使用)。
     * @param[in] caretScreenPos キャレット位置(未使用)。
     * @param[in] caretHeightPx  キャレットの高さ(未使用)。
     * @todo SDL バックエンドで注釈編集に対応する際に、SDL_StartTextInput と
     *       SDL_SetTextInputArea で IME を有効化する。
     */
    void setTextEditing(bool active, Point caretScreenPos, float caretHeightPx) override;

    /**
     * @brief 色選択ダイアログを表示する(SDL バックエンドでは未実装)。
     * @param[in] initialRGB 初期選択する色(未使用)。
     * @return 常に std::nullopt。
     */
    std::optional<uint32_t> showColorPicker(uint32_t initialRGB) override;

    /**
     * @brief 単発タイマーを開始する。
     * @param[in] milliseconds 満了までの時間(ミリ秒)。満了で App::onTimer が呼ばれる。
     * @note タイマーコールバックは別スレッドで走るため、イベント経由で UI スレッドへ渡す。
     */
    void startTimer(unsigned milliseconds) override;

    /// @brief イベントループの終了を要求する。
    void quit() override;

    /// @}

private:
    /// @brief ファイルダイアログ(非同期コールバック)の完了待ちに使う状態。
    struct DialogState;

    /**
     * @brief SDL イベント 1 件を処理する。
     * @param[in] event 処理するイベント。
     */
    void handleEvent(const SDL_Event& event);

    /// @brief 再描画フラグが立っていれば 1 フレーム描画する。
    void renderIfNeeded();

    /**
     * @brief ウィンドウ座標を物理ピクセルへ変換する。
     * @param[in] x ウィンドウ座標の X。
     * @param[in] y ウィンドウ座標の Y。
     * @return 物理ピクセルでの位置(HiDPI のスケールを反映)。
     */
    Point toPixels(float x, float y) const;

    /**
     * @brief SDL のキーコードを KeyCode へ変換する。
     * @param[in] key SDL のキーコード。
     * @param[in] mod 修飾キーの状態。テンキーの +/- の正規化に使う。
     * @return 対応する KeyCode。未対応のキーなら KeyCode::None。
     */
    static KeyCode keyCodeFromSdl(SDL_Keycode key, SDL_Keymod mod);

    /**
     * @brief 非同期ファイルダイアログの完了を待つ。
     * @param[in,out] state 待ち合わせる対象。コールバックが結果を書き込む。
     * @return 選択されたパス。キャンセル時は std::nullopt。
     */
    std::optional<std::filesystem::path> waitForDialog(DialogState& state);

    SDL_Window* window_ = nullptr;
    SDL_Renderer* sdlRenderer_ = nullptr;
    std::unique_ptr<RendererSdl> renderer_;
    App* app_ = nullptr;
    Uint32 eventDecoded_ = 0;  ///< SDL_RegisterEvents で確保したデコード完了イベント
    Uint32 eventTimer_ = 0;    ///< 同・タイマー満了イベント
    SDL_TimerID timerId_ = 0;
    bool running_ = false;
    bool needsRedraw_ = false;
    bool fullscreen_ = false;
    bool dragging_ = false;  ///< 左ドラッグ(パン)中か
    float lastDragX_ = 0;  ///< 直前のドラッグ位置の X(物理ピクセル)
    float lastDragY_ = 0;  ///< 直前のドラッグ位置の Y(物理ピクセル)
};

} // namespace blinker
