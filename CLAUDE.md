# Blinker — 開発ガイド

軽量・高速起動の画像ビューア。C++20。Windows版は Win32 API / Direct2D / WIC で外部ライブラリ依存ゼロの単一exe(約430KB)。Linux/macOS版は SDL3 + stb(third_party/にベンダリング)の `src/sdl` バックエンド(閲覧専用、編集は未対応)。

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
- `/W4 /utf-8 /permissive-` で警告ゼロを維持すること(gcc/clang では `-Wall -Wextra`)

Linux (WSL2) では:

```bash
# WSL2 から /mnt/c/Users/hiroki/work/Blinker で実行
cmake --preset linux-release && cmake --build --preset linux-release
./build/linux-release/tests/core_tests   # 単体テスト
./build/linux-release/blinker <画像 or フォルダ>   # WSLg でウィンドウ表示
```

- SDL3 はシステムに無ければ FetchContent で自動取得・静的リンクされる(初回は数分かかる)
- SDLバックエンドのWindows上でのコンパイル・動作確認は `-DBLINKER_SDL=ON` で可能:
  `cmake -S . -B build/sdl-test -G Ninja -DCMAKE_BUILD_TYPE=Release -DBLINKER_SDL=ON`
  (blinker_sdl.exe ができる。タイトルバー検証の方法はWin版と同じ)

## 不変条件(壊してはならない)

- **依存方向は win/sdl → platform → core の一方向のみ**。`src/core/` はOSヘッダをincludeしてはならない
  (単体テスト対象)。`src/platform/` は抽象インターフェースのヘッダのみ
- **core/platform 層の文字列は UTF-8 の `std::string`**。`std::wstring` は win 層内部のみ。
  Win32 境界の変換は `core/unicode.h`(`utf8ToWide`/`wideToUtf8`/`pathToUtf8`/`pathFromUtf8`、
  純C++実装で単体テスト対象)を使う
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
- パス比較は大文字小文字を無視(Windows準拠)。フォルダ内ソートは Win版が `StrCmpLogicalW`、
  SDL版が core の `naturalCompare`(いずれもエクスプローラと同じ自然順)
- ソースはUTF-8(日本語コメント可)。`/utf-8` フラグ必須
- 設定は `blinker.exe` と同階層の `blinker.ini`(書式: docs/blinker.ini.example)

## 動作確認の方法

GUIアプリのためスクリーンショットは取れないが、タイトルバーに状態が出るので
`Start-Process` + `WScript.Shell` の `AppActivate`/`SendKeys` でキー操作をシミュレートし、
`MainWindowTitle`(`ファイル名 [i/n] ズーム% - Blinker`)の変化で検証できる。
テスト画像はSystem.Drawingで生成可能(過去の検証ではjpg/png/bmp/gif各サイズを生成して確認)。

## リリース手順

**`CMakeLists.txt` の `project(Blinker VERSION x.y.z)` を書き換えて main に push する。以上。**

バージョンの正はこの1行だけ。他はすべて派生物であり、手で書いてはならない:

- `version.h` / `blinker.rc` … `configure_file` で生成(既存)
- **git タグ `vx.y.z`** … `.github/workflows/tag.yml` が push を検知して自動作成
- **GitHub Release + `blinker-x.y.z-win-x64.exe`** … タグ作成を受けて
  `.github/workflows/release.yml` がビルド・テストして添付

タグを手で打たないこと。手で打つとバージョンの正が二か所に増え、drift しうる。
CI は既存タグがあれば何もしないので、CMakeLists.txt の他の行を編集しても安全。
release.yml はタグと CMakeLists.txt の不一致を検知したら失敗する(手打ちタグへの保険)。

release.yml のビルドは Ninja プリセットではなく Visual Studio ジェネレータを使う。
プリセットは vcvars を要求し、CI では第三者製アクションが必要になるため。配布バイナリの
生成経路なのでサプライチェーンは公式アクションのみに絞っている。両ジェネレータの出力は
429,056 バイトで一致(サイズ上の不利はない)。Linux/macOS 版のバイナリ配布は未対応。

git タグを正にする(`git describe` からバージョンを得る)案は採らない。`blinker.rc` の
`FILEVERSION` は MAJOR/MINOR/PATCH を個別の整数で要求するため、git情報の無い環境
(ソースZIP配布、shallow clone)で `0,0,0` が exe のプロパティに焼き込まれてしまう。
