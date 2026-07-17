#pragma once

namespace blinker {

// ユーザー操作の一覧。キー入力やマウス操作は Command に正規化されて App::execute に集まる。
// 機能追加の手順:
//   (1) ここに列挙子を追加
//   (2) App::execute にハンドラを追加
//   (3) keymap.cpp の kCommandNames とデフォルトキーに追加
enum class Command {
    None,
    NextImage,
    PrevImage,
    FirstImage,
    LastImage,
    ZoomIn,
    ZoomOut,
    ZoomFit,
    ZoomActual,
    PanLeft,
    PanRight,
    PanUp,
    PanDown,
    RotateCW,
    RotateCCW,
    ToggleFullscreen,
    OpenFile,
    CopyImage,  // 表示中の画像をクリップボードへ
    CopyPath,   // 表示中の画像のフルパスをクリップボードへ
    Escape,  // フルスクリーン解除。通常時は終了
    Quit,
};

} // namespace blinker
