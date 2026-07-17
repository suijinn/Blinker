# Blinker — 開発ガイド

軽量・高速起動のWindows用画像ビューア。C++20 / Win32 API / Direct2D / WIC。外部ライブラリ依存ゼロの単一exe(約330KB)。

## ビルド・テスト・実行

MSVC環境変数が必要(通常のシェルでは cl/cmake にPATHが通っていない):

```powershell
# 一括実行の例(cmdでvcvars64を通してから実行する)
cmd /c '"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && cmake --preset release && cmake --build --preset release'

# テスト(coreの単体テスト。GUIなしで実行可能)
.\build\release\tests\core_tests.exe

# 実行
.\build\release\blinker.exe <画像ファイル or フォルダ>
```

- CMakeプリセット: `debug` / `release`(Ninja、`build/<preset>/` に出力)
- CMake/NinjaはVS2022同梱のものが使われる(vcvars64がPATHに追加する)
- `/W4 /utf-8 /permissive-` で警告ゼロを維持すること

## アーキテクチャ(詳細は docs/architecture.md)

3層構造。**依存方向は win → platform → core の一方向のみ**:

- `src/core/` — プラットフォーム非依存の純C++20。**OSヘッダをincludeしてはならない**(単体テスト対象)
  - `app.cpp` 状態機械の中心 / `viewport.cpp` ズーム・パン・回転の座標計算 /
    `image_cache.cpp` ワーカースレッド1本の非同期先読み+LRU / `keymap.cpp` キー→Command解決 /
    `image_list.cpp` 一覧と現在位置 / `config.cpp` iniパーサ
- `src/platform/` — 抽象インターフェース(ヘッダのみ): `IImageDecoder` `IRenderer` `IFileSystem`
- `src/win/` — Windows実装: `main_win`(エントリ)/ `window_win`(Win32メッセージ→イベント変換、IAppHost実装)/
  `renderer_d2d` / `decoder_wic` / `file_system_win`

データフローは一方向: 入力 → KeyChord → `Keymap` → `Command` → `App::execute` → 状態更新 → `IAppHost::requestRedraw` → WM_PAINTで描画。

## 機能追加の定型手順

1. `src/core/command.h` の `Command` enum に追加
2. `src/core/app.cpp` の `App::execute` にハンドラ追加(ウィンドウ側の機能が必要なら `IAppHost` にAPIを足し `MainWindow` に実装)
3. `src/core/keymap.cpp` の `kCommandNames`(ini用の名前)とデフォルトキー表 `Keymap::defaults()` に追加
4. `tests/core_tests.cpp` にテスト追加

## スレッドモデル(重要)

- App/Viewport/ImageListはUIスレッド専用。スレッド安全ではない
- デコードは `ImageCache` 内のワーカースレッド1本のみ。UI→ワーカーは `requestNow`/`setPrefetch`、
  ワーカー→UIは `onDecoded` コールバック → `PostMessage(kMsgImageDecoded)` → `App::onDecodeCompleted`
- `DecoderWic` はthread_localでCOM初期化するためどのスレッドからでも呼べる

## 規約・注意点

- 画像ピクセルは常に 32bpp PBGRA(事前乗算)。`DecodedImage` 参照は `shared_ptr` で持ち回る
  (RendererD2DのGPUビットマップキャッシュはshared_ptrをキーにしてアドレス再利用の取り違えを防いでいる)
- 座標はすべて物理ピクセル(D2DはDPI 96固定でDIP=px)。DPI対応はmanifestのPerMonitorV2
- パス比較は大文字小文字を無視(Windows準拠)。フォルダ内ソートは `StrCmpLogicalW`(エクスプローラと同じ自然順)
- ソースはUTF-8(日本語コメント可)。`/utf-8` フラグ必須
- 設定は `blinker.exe` と同階層の `blinker.ini`(書式: docs/blinker.ini.example)

## 動作確認の方法

GUIアプリのためスクリーンショットは取れないが、タイトルバーに状態が出るので
`Start-Process` + `WScript.Shell` の `AppActivate`/`SendKeys` でキー操作をシミュレートし、
`MainWindowTitle`(`ファイル名 [i/n] ズーム% - Blinker`)の変化で検証できる。
テスト画像はSystem.Drawingで生成可能(過去の検証ではjpg/png/bmp/gif各サイズを生成して確認)。
