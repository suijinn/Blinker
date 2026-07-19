# Blinker アーキテクチャ

## 設計目標

1. **軽量・高速起動** — 外部ライブラリなし。Win32 + Direct2D + WIC 直叩き
2. **高速なフォルダ内遷移** — ワーカースレッドによる先読み + LRU キャッシュ
3. **機能追加しやすい** — 全操作を `Command` に一元化した一方向フロー
4. **クロスプラットフォームへの道を残す** — OS 依存コードを `platform` インターフェースの裏に隔離

## 層構造

```
┌───────────────────────────┐  ┌───────────────────────────────┐
│ src/win  (Windows 実装層)  │  │ src/sdl (SDL3 実装層,          │
│  main_win / window_win /   │  │          Linux / macOS)       │
│  renderer_d2d /            │  │  main_sdl / window_sdl /      │
│  decoder_wic / encoder_wic │  │  renderer_sdl / font_stb /    │
│  / wic_factory /           │  │  decoder_stb / encoder_stb /  │
│  file_system_win /         │  │  file_system_posix /          │
│  clipboard_win             │  │  clipboard_sdl                │
└──────────────┬────────────┘  └────────────┬──────────────────┘
               │ 実装・所有                   │ 実装・所有
┌──────────────▼─────────────────────────────▼──────────────────┐
│ src/platform (抽象インターフェース)                              │
│  IRenderer / IImageDecoder / IImageEncoder / IFileSystem /     │
│  IClipboard / IAnnotationRasterizer                            │
└──────────────┬────────────────────────────────────────────────┘
               │ 利用
┌──────────────▼────────────────────────────┐
│ src/core (プラットフォーム非依存・純C++20)   │
│  App / Viewport / ImageList / ImageCache / │
│  Keymap / Config / Command / Dib /         │
│  PixelConvert / Unicode / StrUtil          │
└───────────────────────────────────────────┘
```

- **core は OS ヘッダを一切 include しない**。単体テスト(tests/core_tests.cpp)の対象
- **文字列は core/platform 層では UTF-8 の `std::string` に統一**(`std::wstring` 禁止)。
  Win32 API が UTF-16 を要求する境界(win 層)でのみ `core/unicode.h` の
  `utf8ToWide`/`wideToUtf8` で変換する。パスは `std::filesystem::path` のまま持ち回り、
  表示・比較には `pathToUtf8`/`pathFromUtf8` を使う(変換は純 C++ 実装で単体テスト対象)

## SDL3 バックエンド (Linux / macOS)

`src/sdl` は同じ `platform` 抽象の別実装。CMake オプション `BLINKER_SDL`(非 Windows で
既定 ON)でビルドされ、システムに SDL3 がなければ FetchContent でソース取得する。
デコード/エンコード/文字描画は `third_party/stb` の単一ヘッダ
(stb_image / stb_image_write / stb_truetype、パブリックドメイン)を使う。

| コンポーネント | 対応する win 実装 | 備考 |
|---|---|---|
| `WindowSdl` | `MainWindow` | SDL イベント→App 変換、IAppHost 実装。ファイルダイアログは SDL3 の非同期 API を同期待ちで包む |
| `RendererSdl` | `RendererD2D` | SDL_Renderer。画像はテクスチャ化して ±1 枚キャッシュ、UI 文字は FontStb で CPU 合成 |
| `FontStb` | (DirectWrite) | stb_truetype。Noto CJK 等の候補パスを自動探索(ini の `[view] font_path` で上書き可) |
| `DecoderStb` | `DecoderWic` | stb_image。EXIF 回転は未適用(制限) |
| `EncoderStb` | `EncoderWic` | stb_image_write。PNG/JPEG/BMP |
| `FileSystemPosix` | `FileSystemWin` | `std::filesystem` + core の `naturalCompare`(自然順) |
| `ClipboardSdl` | `ClipboardWin` | テキストは SDL、画像は "image/png" MIME で PNG 受け渡し |
| `AnnotationStub` | `AnnotationD2D` | **未実装**。編集メニュー(コンテキストメニュー・テキスト入力・色選択)も未対応のため、SDL 版は閲覧専用 |

SDL 版の既知の制限: 注釈編集ができない(showContextMenu 等が nullopt を返す)、
EXIF 回転が効かない、対応形式が stb_image の範囲(WebP/HEIC/AVIF/TIFF 等は不可)。

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
トリミングは core の `cropImage` で `current_` を差し替え、図形・テキストは
**非破壊の注釈オブジェクト**(`std::vector<AnnotationSpec>`)として App が保持する。
注釈は描画時に `AnnotationsView` として RendererD2D へ渡されベクター描画で重なり
(`current_` のピクセルは変更しない)、保存 (Ctrl+S)・コピー (Ctrl+C) 時にだけ
`App::compositeImage` が `IAnnotationRasterizer` + `blendOverlay` で合成する。
トリミングは注釈座標を平行移動してオブジェクトのまま維持する。
メニューは階層構造(`MenuItem` の木、選択結果は末端項目の深さ優先通し番号)で、
設定系の項目(線の太さ・文字サイズ・色 = `IAppHost::showColorPicker`)を
選んだ場合は選択領域を保ったままメニューを再表示して続けて編集を選べる
(設定は新規作成の既定値)。

追加済みの注釈は左クリックで選択して編集できる(ヒットテスト・回転・リサイズの幾何は
core の `annotation_edit.cpp`、純粋関数、単体テスト対象):
左ドラッグで移動、四隅・辺のハンドルでサイズ変更(Shift で縦横比維持。回転中は
反対側のアンカーを固定。Line/Arrow は端点ドラッグ、Text は幅のみで高さは
ドラッグ確定時に実測へ正規化)、選択枠上の回転ハンドルで自由回転(Shift で 15° スナップ)、
右クリックでオブジェクトメニュー(回転角度プリセット・太さ・文字サイズ・色・削除)、
テキストはダブルクリックで再編集(`IAppHost::showTextInput` に初期値を渡す)。
Ctrl+Z で1段階ずつ取り消せる(履歴は画像 + 注釈一覧のスナップショット、上限10)。
保存は Ctrl+S のみで元ファイルは自動では書き換えない。

## 主要コンポーネント

| コンポーネント | 責務 |
|---|---|
| `App` | 状態機械の中心。Command を受けて状態更新、host へ再描画依頼。ステータスバー (`StatusBarView`) とサイドバー (`SidebarView`、可視範囲の項目のみ) の表示内容もここで組み立てる。貼り付け画像はフォルダ一覧から独立した表示状態(`clipboardImage_`)で持ち、移動系コマンドで一覧表示へ戻る。編集(右ドラッグ選択→トリミング・図形・テキスト、`SelectionView`・注釈オブジェクトの選択/移動/回転ドラッグ状態・undo 履歴)もここで管理し、画像切替で破棄する |
| `Viewport` | ズーム/パン/フィット/回転の座標変換(純粋計算、テスト容易) |
| `ImageList` | フォルダ内画像の一覧・現在位置・先読み候補の順序付け |
| `ImageCache` | ワーカースレッド1本で非同期デコード。LRU(既定: 8枚 or 512MB) |
| `Keymap` | KeyChord → Command。デフォルト表 + ini 上書き |
| `MainWindow` | Win32 メッセージ変換、フルスクリーン、ダイアログ(開く・保存・色選択)・編集メニュー(サブメニュー対応)・複数行テキスト入力(IAppHost 実装) |
| `RendererD2D` | BGRA ピクセル → ID2D1Bitmap(±1枚をGPU側にキャッシュ)して描画。注釈オブジェクト(`AnnotationsView`、選択枠・回転ハンドル含む)と選択領域のラバーバンド(`SelectionView`)もここで描く。図形の描画コードは焼き込みと共通(win/annotation_draw)。サイドバー・ステータスバーの文字は DirectWrite |
| `DecoderWic` | WIC で 32bpp PBGRA に統一デコード。EXIF 回転適用、16384px 超は縮小 |
| `EncoderWic` | WIC で PNG/JPEG/BMP 保存 (Ctrl+S)。PNG は逆乗算してアルファ保持、JPEG/BMP は白背景に合成 |
| `ClipboardWin` | クリップボード読み書き。書き込みは CF_DIBV5(アルファ)+ CF_DIB(白合成24bpp)の2形式。読み取り (Ctrl+V) は CF_DIBV5 優先で、DIB → PBGRA 変換は core の `imageFromDib`(純粋関数、単体テスト対象) |
| `AnnotationD2D` | 図形(矩形・楕円・矢印・直線)とテキスト(複数行可)を D2D/DirectWrite で WIC ビットマップへ AA 描画し、PBGRA overlay として返す(`IAnnotationRasterizer` 実装)。描画コードはライブ表示と共通の `win/annotation_draw` を使い、`AnnotationSpec::angleDeg` によるバウンディングボックス中心周りの回転にも対応。テキスト注釈の実測サイズ取得(`App::measureTextExtent`)にも使われる。トリミング・合成は core の `edit.cpp`(`cropImage` / `blendOverlay`)、注釈のヒットテスト・回転幾何は core の `annotation_edit.cpp`(いずれも純粋関数、単体テスト対象) |

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

- ごみ箱削除 / EXIF 情報表示
- アニメ GIF 再生(WIC の全フレームデコード + タイマー)
- stb_image 系フォールバックデコーダ(`IImageDecoder` の別実装)※ SDL 版では実装済み (decoder_stb)
- SDL 版の編集対応(コンテキストメニュー・テキスト入力・色選択の自前 UI + CPU ラスタライザ)
- SDL 版の EXIF 回転対応(JPEG の Orientation タグを自前パース)
