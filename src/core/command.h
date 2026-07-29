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
    TogglePlay,        ///< アニメーションの再生 / 一時停止
    NextFrame,         ///< 次のフレーム / ページへ(ファイルは移動しない)
    PrevFrame,         ///< 前のフレーム / ページへ(同上)
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
    CopyFile,          ///< 表示中の画像のファイル実体をクリップボードへ(エクスプローラに貼り付け可能)
    CopyOcrText,       ///< 表示中の画像から文字を認識してクリップボードへ
    PasteImage,        ///< クリップボードの画像を表示(次/前でフォルダ一覧に戻る)
    PasteObject,       ///< クリップボードの画像を表示中の画像の上へオブジェクトとして貼る
    SaveImage,         ///< 表示中の画像を元のファイルへ上書き保存(確認あり)
    SaveImageAs,       ///< 表示中の画像を名前を付けて保存 (PNG/JPEG/BMP)
    PrintImage,        ///< 表示中の画像を印刷する(印刷ダイアログを出す)
    ResizeImage,       ///< 表示中の画像をリサイズする(倍率・長辺のメニューを出す)
    SortByName,        ///< 一覧を名前順に。既に名前順なら昇順 / 降順を反転する
    SortByDate,        ///< 一覧を更新日時順に(同上)
    SortBySize,        ///< 一覧をファイルサイズ順に(同上)
    SortByExtension,   ///< 一覧を種類(拡張子)順に(同上)
    ToggleSortDescending,  ///< 一覧の昇順 / 降順を反転する
    CycleSortKey,      ///< 一覧の並び替えキーを順に切り替える
    ToggleRecursive,   ///< サブフォルダの画像も一覧に含めるかを切り替える
    Undo,              ///< 直前の編集(トリミング・図形・テキスト)を取り消す
    Redo,              ///< 取り消した編集をやり直す
    DeleteAnnotation,  ///< 選択中の注釈オブジェクトを削除
    SelectToolCrop,    ///< 編集ドラッグのツールをトリミングに切り替える
    SelectToolRect,    ///< 編集ドラッグのツールを矩形に切り替える
    SelectToolEllipse, ///< 編集ドラッグのツールを楕円に切り替える
    SelectToolArrow,   ///< 編集ドラッグのツールを矢印に切り替える
    SelectToolLine,    ///< 編集ドラッグのツールを直線に切り替える
    SelectToolPen,     ///< 編集ドラッグのツールを手書き(ペン)に切り替える
    SelectToolMarker,  ///< 編集ドラッグのツールをマーカー(半透明の手書き)に切り替える
    SelectToolNumber,  ///< 編集ドラッグのツールを連番マーカーに切り替える
    SelectToolText,    ///< 編集ドラッグのツールをテキストに切り替える
    SelectToolOcr,     ///< 編集ドラッグのツールを文字認識(範囲指定)に切り替える
    ToggleSidebar,     ///< ファイル名一覧のサイドバーの表示切り替え
    ToggleStatusBar,   ///< ステータスバーの表示切り替え
    ToggleHelp,        ///< サイドバーにキー一覧(ヘルプ)を表示 / 非表示
    Escape,            ///< フルスクリーン解除。通常時は終了
    Quit,              ///< アプリケーションを終了
};

} // namespace blinker
