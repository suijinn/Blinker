# Blinker

軽量・高速起動の Windows 用画像ビューア。C++20 / Win32 API / Direct2D / WIC 製、外部依存ゼロの単一 exe。

## 特徴

- **軽快**: 数百 KB の単一実行ファイル。起動即表示、フォルダ内の画像を先読みして瞬時に遷移
- **キーボード中心**: すべての操作にショートカットキー。`blinker.ini` で自由に変更可能
- **対応形式**: JPEG / PNG / BMP / GIF / TIFF / ICO / JXR。Windows 11 なら HEIC / WebP / AVIF も(OS コーデック経由)
- **拡張しやすい設計**: プラットフォーム非依存の core 層と Win32 実装層を分離(詳細は [docs/architecture.md](docs/architecture.md))

## ビルド

必要環境: Visual Studio 2022(「C++ によるデスクトップ開発」ワークロード)

```powershell
# "Developer PowerShell for VS 2022" で実行
cmake --preset release
cmake --build --preset release
# 生成物: build/release/blinker.exe

# テスト
ctest --preset release
```

## 使い方

```
blinker.exe <画像ファイル | フォルダ>
```

画像ファイルへのドラッグ&ドロップ、「プログラムから開く」にも対応。

### デフォルトキーバインド

| 操作 | キー |
|---|---|
| 次 / 前の画像 | `→` `Space` `PageDown` / `←` `Backspace` `PageUp` |
| 最初 / 最後の画像 | `Home` / `End` |
| ズームイン / アウト | `+` / `-`、マウスホイール(カーソル位置基準) |
| ウィンドウにフィット / 等倍 | `0` / `1` |
| スクロール(パン) | `Ctrl+矢印`、マウス左ドラッグ |
| 回転(表示のみ) | `R`(右90°)/ `Shift+R`(左90°) |
| フルスクリーン | `F11` `Enter` |
| ファイルを開く | `Ctrl+O` |
| 画像をクリップボードへコピー | `Ctrl+C` |
| 画像のパスをコピー | `Shift+Ctrl+C` |
| ステータスバー表示切替 | `B` |
| 終了 | `Q`、`Esc`(フルスクリーン中は解除) |

### 設定

`blinker.exe` と同じフォルダに `blinker.ini` を置くと、キーバインドや背景色、
タイトルバーのテーマ(OS設定追従 / ダーク / ライト)を変更できる。
書式は [docs/blinker.ini.example](docs/blinker.ini.example) を参照。

## ライセンス / 開発メモ

個人プロジェクト。設計・拡張方法は [docs/architecture.md](docs/architecture.md) を参照。
