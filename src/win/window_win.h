#pragma once

#include <windows.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "core/app.h"
#include "win/renderer_d2d.h"

/**
 * @file window_win.h
 * @brief メインウィンドウ(Windows 実装)。
 */

namespace blinker {

/**
 * @brief メインウィンドウ。
 *
 * Win32 メッセージを App のコマンド/イベントに変換し、IAppHost を実装する。
 */
class MainWindow final : public IAppHost {
public:
    /// @brief ImageCache のワーカースレッドからのデコード完了通知に使うウィンドウメッセージ。
    static constexpr UINT kMsgImageDecoded = WM_APP + 1;

    /**
     * @brief ウィンドウクラスを登録してウィンドウを作成・表示する。
     * @param[in] hinstance   モジュールインスタンスハンドル。
     * @param[in] showCommand ShowWindow に渡す表示状態(WinMain の nCmdShow)。
     * @param[in] darkTitleBar true ならタイトルバーをダーク配色にする。
     * @return 作成できたら true。失敗時は false。
     */
    bool create(HINSTANCE hinstance, int showCommand, bool darkTitleBar);

    /**
     * @brief イベントの転送先となる App を結び付ける。
     * @param[in] app 転送先の App。本オブジェクトより長生きすること。
     */
    void attachApp(App* app);

    /**
     * @brief ウィンドウハンドルを返す。
     * @return ウィンドウハンドル。create 前は nullptr。
     */
    HWND hwnd() const { return hwnd_; }

    /// @name IAppHost の実装
    /// @{

    /// @brief クライアント領域を無効化して再描画を要求する。
    void requestRedraw() override;

    /**
     * @brief ウィンドウタイトルを設定する。
     * @param[in] title 設定するタイトル(UTF-8)。内部で UTF-16 へ変換する。
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
     * @brief ファイルを開くダイアログ (IFileOpenDialog) を表示する。
     * @return 選択されたパス。キャンセル時は std::nullopt。
     */
    std::optional<std::filesystem::path> showOpenDialog() override;

    /**
     * @brief 名前を付けて保存ダイアログ (IFileSaveDialog) を表示する。
     * @param[in] defaultFileName 初期表示するファイル名(UTF-8)。
     * @return 選択されたパス(拡張子付き)。キャンセル時は std::nullopt。
     */
    std::optional<std::filesystem::path> showSaveDialog(
        const std::string& defaultFileName) override;

    /**
     * @brief ポップアップメニューを表示する(モーダル)。
     * @param[in] items     メニュー構造。
     * @param[in] screenPos 表示位置(クライアント座標)。
     * @return 選択された末端項目の index。キャンセル時は std::nullopt。
     */
    std::optional<size_t> showContextMenu(const std::vector<MenuItem>& items,
                                          Point screenPos) override;

    /**
     * @brief テキスト入力ダイアログ(複数行)を表示する。
     * @param[in] initial 初期値として表示する文字列(UTF-8、改行は LF)。
     * @return 入力された文字列(UTF-8、改行は LF)。キャンセル時は std::nullopt。
     */
    std::optional<std::string> showTextInput(const std::string& initial) override;

    /**
     * @brief 色選択ダイアログ (ChooseColor) を表示する。
     * @param[in] initialRGB 初期選択する色(0xRRGGBB)。
     * @return 選択された色(0xRRGGBB)。キャンセル時は std::nullopt。
     */
    std::optional<uint32_t> showColorPicker(uint32_t initialRGB) override;

    /**
     * @brief 単発タイマーを開始する。
     * @param[in] milliseconds 満了までの時間(ミリ秒)。満了で App::onTimer が呼ばれる。
     */
    void startTimer(unsigned milliseconds) override;

    /// @brief メッセージループの終了を要求する (PostQuitMessage)。
    void quit() override;

    /// @}

private:
    /**
     * @brief ウィンドウプロシージャ(静的エントリ)。
     * @param[in] hwnd 対象のウィンドウハンドル。
     * @param[in] msg  メッセージ ID。
     * @param[in] wp   メッセージ固有のパラメータ。
     * @param[in] lp   メッセージ固有のパラメータ。
     * @return メッセージの処理結果。
     */
    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    /**
     * @brief インスタンス側のメッセージ処理。
     * @param[in] msg メッセージ ID。
     * @param[in] wp  メッセージ固有のパラメータ。
     * @param[in] lp  メッセージ固有のパラメータ。
     * @return メッセージの処理結果。
     */
    LRESULT handleMessage(UINT msg, WPARAM wp, LPARAM lp);

    /**
     * @brief 左ボタン押下を処理し、パン/注釈ドラッグの開始を判断する。
     * @param[in] pt 押下位置(クライアント座標)。
     */
    void handleLeftDown(POINT pt);

    /// @brief WM_PAINT を処理し、App のスナップショットをレンダラへ渡す。
    void onPaint();

    /**
     * @brief クライアント領域のサイズ変更を処理する。
     * @param[in] width  新しい幅(物理ピクセル)。
     * @param[in] height 新しい高さ(物理ピクセル)。
     */
    void onSize(uint32_t width, uint32_t height);

    /**
     * @brief キー入力を App へ転送する。
     * @param[in] vk 仮想キーコード。
     * @return App がバインドを持ち実行したら true。
     */
    bool handleKey(WPARAM vk);

    /**
     * @brief ドラッグ＆ドロップされたファイルを開く。
     * @param[in] wp WM_DROPFILES の wParam (HDROP)。
     */
    void onDropFiles(WPARAM wp);

    /**
     * @brief 仮想キーコードを KeyCode へ変換する。
     * @param[in] vk 仮想キーコード。
     * @return 対応する KeyCode。未対応のキーなら KeyCode::None。
     */
    static KeyCode keyCodeFromVirtualKey(WPARAM vk);

    HWND hwnd_ = nullptr;
    App* app_ = nullptr;
    std::unique_ptr<RendererD2D> renderer_;
    bool fullscreen_ = false;
    WINDOWPLACEMENT savedPlacement_{sizeof(WINDOWPLACEMENT)};
    LONG savedStyle_ = 0;
    bool dragging_ = false;
    bool rightDragging_ = false;  ///< 編集領域の選択中(右ボタン)
    POINT lastDragPos_{};
    bool trackingMouseLeave_ = false;
};

} // namespace blinker
