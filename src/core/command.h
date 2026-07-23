#pragma once

/**
 * @file command.h
 * @brief ユーザー操作を表す Command 列挙。
 */

namespace blinker {

/**
 * @brief ユーザー操作の一覧。
 *
 * キー入力やマウス操作は Command に正規化されて App::execute に集まる。
 *
 * 機能追加の手順:
 *   -# ここに列挙子を追加
 *   -# App::execute にハンドラを追加
 *   -# keymap.cpp の kCommandNames とデフォルトキーに追加
 */
enum class Command {
    None,              ///< 未割り当て(バインドなし)
    NextImage,         ///< 次の画像へ
    PrevImage,         ///< 前の画像へ
    FirstImage,        ///< 一覧の先頭へ
    LastImage,         ///< 一覧の末尾へ
    ZoomIn,            ///< 拡大
    ZoomOut,           ///< 縮小
    ZoomFit,           ///< ウィンドウにフィット
    ZoomActual,        ///< 等倍表示
    PanLeft,           ///< 左へパン
    PanRight,          ///< 右へパン
    PanUp,             ///< 上へパン
    PanDown,           ///< 下へパン
    RotateCW,          ///< 時計回りに 90 度回転
    RotateCCW,         ///< 反時計回りに 90 度回転
    ToggleFullscreen,  ///< フルスクリーンの切り替え
    OpenFile,          ///< ファイルを開くダイアログ
    CopyImage,         ///< 表示中の画像をクリップボードへ
    CopyPath,          ///< 表示中の画像のフルパスをクリップボードへ
    PasteImage,        ///< クリップボードの画像を表示(次/前でフォルダ一覧に戻る)
    SaveImageAs,       ///< 表示中の画像を名前を付けて保存 (PNG/JPEG/BMP)
    Undo,              ///< 直前の編集(トリミング・図形・テキスト)を取り消す
    DeleteAnnotation,  ///< 選択中の注釈オブジェクトを削除
    SelectToolCrop,    ///< 右ドラッグのツールをトリミングに切り替える
    SelectToolRect,    ///< 右ドラッグのツールを矩形に切り替える
    SelectToolEllipse, ///< 右ドラッグのツールを楕円に切り替える
    SelectToolArrow,   ///< 右ドラッグのツールを矢印に切り替える
    SelectToolLine,    ///< 右ドラッグのツールを直線に切り替える
    SelectToolText,    ///< 右ドラッグのツールをテキストに切り替える
    ToggleSidebar,     ///< ファイル名一覧のサイドバーの表示切り替え
    ToggleStatusBar,   ///< ステータスバーの表示切り替え
    ToggleHelp,        ///< サイドバーにキー一覧(ヘルプ)を表示 / 非表示
    Escape,            ///< フルスクリーン解除。通常時は終了
    Quit,              ///< アプリケーションを終了
};

} // namespace blinker
