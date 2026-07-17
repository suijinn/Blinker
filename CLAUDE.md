# Blinker — 開発ガイド

軽量・高速起動のWindows用画像ビューア。C++20 / Win32 API / Direct2D / WIC。外部ライブラリ依存ゼロの単一exe(約330KB)。

設計の詳細(層構造・コンポーネントの責務・データフロー・起動シーケンス)の正は
[docs/architecture.md](docs/architecture.md)。ここにはビルド方法と、コードを触るたびに必要になる
不変条件・規約だけを書く。

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

## 不変条件(壊してはならない)

- **依存方向は win → platform → core の一方向のみ**。`src/core/` はOSヘッダをincludeしてはならない
  (単体テスト対象)。`src/platform/` は抽象インターフェースのヘッダのみ
- **データフローは一方向**: 入力 → KeyChord → `Keymap` → `Command` → `App::execute` → 状態更新 →
  `IAppHost::requestRedraw` → WM_PAINTで描画
- **スレッドモデル**: App/Viewport/ImageListはUIスレッド専用でスレッド安全ではない。
  デコードは `ImageCache` 内のワーカースレッド1本のみ。UI→ワーカーは `requestNow`/`setPrefetch`、
  ワーカー→UIは `onDecoded` → `PostMessage(kMsgImageDecoded)` → `App::onDecodeCompleted`。
  `DecoderWic` はthread_localでCOM初期化するためどのスレッドからでも呼べる
- **機能追加は定型手順に従う**: `Command` enum追加 → `App::execute` にハンドラ →
  `keymap.cpp`(`kCommandNames` とデフォルトキー表)→ `tests/core_tests.cpp` にテスト。
  詳細は architecture.md「機能追加の手順」

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
