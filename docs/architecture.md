# Blinker アーキテクチャ

## 設計目標

1. **軽量・高速起動** — 外部ライブラリなし。Win32 + Direct2D + WIC 直叩き
2. **高速なフォルダ内遷移** — ワーカースレッドによる先読み + LRU キャッシュ
3. **機能追加しやすい** — 全操作を `Command` に一元化した一方向フロー
4. **クロスプラットフォームへの道を残す** — OS 依存コードを `platform` インターフェースの裏に隔離

## 層構造

```
┌───────────────────────────────────────────┐
│ src/win  (Windows 実装層)                  │
│  main_win / window_win / renderer_d2d /    │
│  decoder_wic / encoder_wic / wic_factory / │
│  file_system_win / clipboard_win           │
└──────────────┬────────────────────────────┘
               │ 実装・所有
┌──────────────▼────────────────────────────┐
│ src/platform (抽象インターフェース)          │
│  IRenderer / IImageDecoder /               │
│  IImageEncoder / IFileSystem / IClipboard  │
└──────────────┬────────────────────────────┘
               │ 利用
┌──────────────▼────────────────────────────┐
│ src/core (プラットフォーム非依存・純C++20)   │
│  App / Viewport / ImageList / ImageCache / │
│  Keymap / Config / Command / Dib /         │
│  PixelConvert                              │
└───────────────────────────────────────────┘
```

- **core は OS ヘッダを一切 include しない**。単体テスト(tests/core_tests.cpp)の対象
- Linux/Mac 対応は `platform` の別実装(例: SDL バックエンド)を足し、エントリポイントを追加するだけでよい構造

## データフロー(一方向)

```
入力 (WM_KEYDOWN / ホイール / D&D)
  → MainWindow が KeyChord / イベントへ変換
  → Keymap が Command を解決
  → App::execute が状態を更新 (ImageList / Viewport)
  → IAppHost 経由で再描画・タイトル更新を依頼
  → WM_PAINT で RendererD2D が App の状態を描画
```

マウスの編集操作(右ドラッグ)も同じ形で App に集まる:
`onRightDragStart/Move/End` → `IAppHost::showContextMenu` で機能選択 →
トリミングは core の `cropImage`、図形・テキストは `IAnnotationRasterizer` で
ラスタライズして `blendOverlay` で合成 → `current_` を差し替えて再描画依頼。
メニューは階層構造(`MenuItem` の木、選択結果は末端項目の深さ優先通し番号)で、
設定系の項目(線の太さ・文字サイズ・回転角度・色 = `IAppHost::showColorPicker`)を
選んだ場合は選択領域を保ったままメニューを再表示して続けて編集を選べる。
編集は表示中画像のコピーに対して行い(キャッシュとは共有しない)、Ctrl+Z で
1段階ずつ取り消せる(履歴は画像スナップショット、上限10)。保存は Ctrl+S のみで
元ファイルは自動では書き換えない。

## 主要コンポーネント

| コンポーネント | 責務 |
|---|---|
| `App` | 状態機械の中心。Command を受けて状態更新、host へ再描画依頼。ステータスバー (`StatusBarView`) とサイドバー (`SidebarView`、可視範囲の項目のみ) の表示内容もここで組み立てる。貼り付け画像はフォルダ一覧から独立した表示状態(`clipboardImage_`)で持ち、移動系コマンドで一覧表示へ戻る。編集(右ドラッグ選択→トリミング・図形・テキスト、`SelectionView`・undo 履歴)もここで管理し、画像切替で破棄する |
| `Viewport` | ズーム/パン/フィット/回転の座標変換(純粋計算、テスト容易) |
| `ImageList` | フォルダ内画像の一覧・現在位置・先読み候補の順序付け |
| `ImageCache` | ワーカースレッド1本で非同期デコード。LRU(既定: 8枚 or 512MB) |
| `Keymap` | KeyChord → Command。デフォルト表 + ini 上書き |
| `MainWindow` | Win32 メッセージ変換、フルスクリーン、ダイアログ(開く・保存・色選択)・編集メニュー(サブメニュー対応)・複数行テキスト入力(IAppHost 実装) |
| `RendererD2D` | BGRA ピクセル → ID2D1Bitmap(±1枚をGPU側にキャッシュ)して描画。選択領域のラバーバンド(`SelectionView`)もここで描く。サイドバー・ステータスバーの文字は DirectWrite |
| `DecoderWic` | WIC で 32bpp PBGRA に統一デコード。EXIF 回転適用、16384px 超は縮小 |
| `EncoderWic` | WIC で PNG/JPEG/BMP 保存 (Ctrl+S)。PNG は逆乗算してアルファ保持、JPEG/BMP は白背景に合成 |
| `ClipboardWin` | クリップボード読み書き。書き込みは CF_DIBV5(アルファ)+ CF_DIB(白合成24bpp)の2形式。読み取り (Ctrl+V) は CF_DIBV5 優先で、DIB → PBGRA 変換は core の `imageFromDib`(純粋関数、単体テスト対象) |
| `AnnotationD2D` | 図形(矩形・楕円・矢印・直線)とテキスト(複数行可)を D2D/DirectWrite で WIC ビットマップへ AA 描画し、PBGRA overlay として返す(`IAnnotationRasterizer` 実装)。`AnnotationSpec::angleDeg` によるバウンディングボックス中心周りの回転にも対応。トリミング・合成は core の `edit.cpp`(`cropImage` / `blendOverlay`、純粋関数、単体テスト対象) |

## スレッドモデル

- **UI スレッド**: メッセージループ、描画、App の全状態。App はスレッド安全ではない
- **デコードワーカー (ImageCache 内の1本)**: `IImageDecoder::decode` の実行のみ
- 境界は 2 箇所だけ:
  - UI → ワーカー: `ImageCache::requestNow / setPrefetch`(ミューテックス保護のキュー)
  - ワーカー → UI: `onDecoded` コールバック → `PostMessage(kMsgImageDecoded)` → `App::onDecodeCompleted`

## 起動シーケンス(高速化の要)

1. `wWinMain`: ウィンドウ生成(即表示、この時点で操作可能)
2. コマンドライン引数の画像を `ImageCache::requestNow` で即デコード開始(フォルダ列挙を待たない)
3. UI スレッドでフォルダ列挙(自然順ソート)→ `ImageList` 確定
4. デコード完了通知で表示、隣接画像の先読み開始

## 機能追加の手順(例: スライドショー)

1. `core/command.h` の `Command` に `ToggleSlideshow` を追加
2. `core/app.cpp` の `App::execute` にハンドラを追加(タイマーは IAppHost に API を足す)
3. `core/keymap.cpp` の `kCommandNames`(ini 名)とデフォルトキー表に追加
4. 必要なら `tests/core_tests.cpp` にテストを追加

## v0.2 以降の候補

- スライドショー / サムネイル一覧 / ごみ箱削除 / EXIF 情報表示
- アニメ GIF 再生(WIC の全フレームデコード + タイマー)
- stb_image 系フォールバックデコーダ(`IImageDecoder` の別実装)
- SDL3 バックエンドによる Linux / macOS 対応
