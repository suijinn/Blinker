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
│  clipboard_win /           │  │  clipboard_sdl /              │
│  printer_win /             │  │  printer_stub                 │
│  print_winrt / winrt_abi   │  │                               │
└──────────────┬────────────┘  └────────────┬──────────────────┘
               │ 実装・所有                   │ 実装・所有
┌──────────────▼─────────────────────────────▼──────────────────┐
│ src/platform (抽象インターフェース)                              │
│  IRenderer / IImageDecoder / IImageEncoder / IFileSystem /     │
│  IClipboard / IAnnotationRasterizer / IOcrEngine / IPrinter    │
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
| `RendererSdl` | `RendererD2D` | SDL_Renderer。画像はテクスチャ化して ±1 枚キャッシュ(D2D 版と同じく枚数とバイト数の両方で上限)、UI 文字は FontStb で CPU 合成。レンダラの最大テクスチャサイズを超える画像は `downscaleToFit` で縮小して載せる(テクスチャ座標は 0〜1 なので描画側は影響を受けない) |
| `FontStb` | (DirectWrite) | stb_truetype。Noto CJK 等の候補パスを自動探索(ini の `[view] font_path` で上書き可) |
| `DecoderStb` | `DecoderWic` | stb_image。EXIF Orientation は core の `readExifOrientation` で自前に解析して適用(JPEG の APP1 / PNG の eXIf) |
| `EncoderStb` | `EncoderWic` | stb_image_write。PNG/JPEG/BMP(JPEG 品質は `EncodeOptions`) |
| `FileSystemPosix` | `FileSystemWin` | `std::filesystem` + core の `naturalCompare`(自然順) |
| `ClipboardSdl` | `ClipboardWin` | テキストは SDL、画像は "image/png" MIME で PNG 受け渡し |
| `PrinterStub` | `PrinterWin` (+ `print_winrt` のモダン印刷 UI) | **未実装**。印刷 (Ctrl+P) は「対応していない」旨をステータスバーに出すだけ(Linux では CUPS、macOS では Cocoa の印刷 API が要り、外部依存ゼロで賄えないため) |
| `AnnotationStub` | `OcrEngineWinrt` | Windows.Media.Ocr による文字認識(`IOcrEngine` 実装)。**WinRT を C++/WinRT ではなく ABI で直接呼び、`combase.dll` は `LoadLibrary` で遅延解決する**。`windowsapp.lib` を静的リンクすると exe のインポートが増え、OCR を使わない起動でも DLL が読まれるため。認識器の生成も最初の実行まで遅延する。上限を超える画像は WIC で縮小し、座標は元のスケールへ戻す。文字が小さいときは 1 回目の行高から拡大率を決めて読み直す 2 パス構成(判断は core の `ocrRetryUpscale`)|
| `AnnotationD2D` | **未実装**(ラスタライズ・テキスト計測とも)。ツールメニュー(コンテキストメニュー・色選択)やテキストのインプレース編集も未対応のため、SDL 版は閲覧専用 |

SDL 版の既知の制限: 印刷ができない、注釈編集ができない(`WindowSdl` は編集役のボタン
(`App::mouseRole` が `MouseRole::Edit` を返すほう)のイベントを App へ渡さない。
ツールメニューも注釈の描画も無いため、繋ぐと見えない注釈だけが増える)、
対応形式が stb_image の範囲(WebP/HEIC/AVIF/TIFF 等は不可)、
取り込み時の大きさに上限が無い(Win 版の 16384px 相当のガードが無いので、巨大画像では
stb_image のメモリ確保が失敗して「読み込み失敗」になる。描画時のテクスチャ上限は
`RendererSdl` が縮小して吸収するので、確保できた画像は必ず表示される)。

## データフロー(一方向)

```
入力 (WM_KEYDOWN / ホイール / サイドボタン / D&D)
  → MainWindow が KeyChord / MouseChord / イベントへ変換
  → Keymap / Mousemap が Command を解決
  → App::execute が状態を更新 (ImageList / Viewport)
  → IAppHost 経由で再描画・タイトル更新を依頼
  → WM_PAINT で RendererD2D が App の状態を描画
```

マウス操作も同じ形で App に集まる。ウィンドウ層は**物理ボタン**(`MouseButton`)を
そのまま `App::onMouseDown` / `onMouseUp` / `onMouseMove` へ渡すだけで、どちらのボタンが
何をするかは App が `mouseRole` で振り分ける(下記「マウスボタンの役割」)。
編集役のボタンのドラッグは **現在のツール**(`EditTool`、`EditStyle::tool`)を選択領域へ
適用し、図形・テキストを **非破壊の注釈オブジェクト**
(`std::vector<AnnotationSpec>`)として App が保持する。
注釈は描画時に `AnnotationsView` として RendererD2D へ渡されベクター描画で重なり
(`current_` のピクセルは変更しない)、保存 (Ctrl+S)・コピー (Ctrl+C) 時にだけ
`App::compositeImage` が `IAnnotationRasterizer` + `blendOverlay` で合成する。

**トリミングはツールではない**(`App::cropToSelection`)。範囲は `Kind::Rect` の注釈として
作り、その右クリックメニュー(または `Command::CropToSelection`)から core の `cropImage` で
`current_` を差し替える。ドラッグを離した時点で確定させないのは、範囲の微調整に
注釈のリサイズハンドル・移動・Shift の縦横比維持をそのまま使うため。切り出したら範囲に
使った矩形は消し、残りの注釈は座標を平行移動してオブジェクトのまま維持する
(消去と切り出しは同じ `pushUndo` の 1 段に入るので Ctrl+Z 一発で両方戻る)。
切り出す範囲の丸めとクランプは core の `cropRectFor` に閉じてあり、**メニューの表示可否・
見出しに出す寸法・実行のすべてが同じ関数を通る**(判定を二重に持たないため)。
回転した矩形(`angleDeg != 0`)ではメニューに出さない ―― 切り出しは軸平行にしか定義されて
おらず、見えている枠と切れる範囲が食い違うため(同じメニューの「回転角度 → 0°」で戻せる)。
**リサイズ (`App::applyResize`) もトリミングと同じ破壊的編集**で、`resizeImage` で
`current_` を差し替え、注釈は `scaleAnnotation` で同じ倍率へ追従させる(焼き込まない)。
大きさは**メニューのプリセット**(倍率・長辺)から選ぶ ―― 数値入力ダイアログは持たない。
`IAppHost` のダイアログはすべて OS 提供のもので賄っており、自前のダイアログを 1 つ足すと
その線を越えるため。上書き保存の可否は `DecodedImage::sourceWidth/Height` の引き継ぎ規則で
決まる(取り込み時に縮小された画像はリサイズしても元の画素を持たないので拒否のまま、
等倍で取り込んだ画像は通常どおり確認だけで通る)。

**ツールは事前に選ぶ**(動詞 → 目的語)。同じ範囲に対して何をするかを後から選ぶ形では、
矢印を5本引くような反復のたびにメニューを往復することになり、ドラッグ中に実物を
見せられないため。切り替えは注釈のない場所での右クリック(ドラッグなし)で開く
ツールメニュー(入れ替え設定にかかわらず常に右クリック)、または
`Command::SelectTool*`(既定のキーは持たず blinker.ini で割り当て)。
起動時のツールは blinker.ini の `[edit] tool`。
ツールメニューは階層構造(`MenuItem` の木、選択結果は末端項目の深さ優先通し番号)で、
組み立ては `menu.h` の純関数(状態 → 項目の木 + 末端項目の一覧)、表示と適用が `App`。
設定系の項目(線の太さ・文字サイズ・フォント・色 = `IAppHost::showColorPicker`)を
選んだ場合はメニューを再表示し、設定を整えてからツールを選べる(設定は新規作成の既定値)。
選択中のツールとこれらの設定は `EditStyle` が持ち、新規注釈へ写すのも
`EditStyle::applyTo`(既にある注釈の書式変更はここを通らず、対象を直接触る)。
フォントの候補は `IAnnotationRasterizer::hasFontFamily` で実在するものだけへ絞る
(起動時には呼ばず、メニューを開いたときだけ問い合わせる)。

ドラッグ中は `App::makeAnnotationSpec` が確定後と同じ `AnnotationSpec` を組み立て、
`AnnotationsView::preview` として実物をプレビュー描画する。形の定まらない
テキスト・文字認識だけは従来どおりラバーバンド (`SelectionView`) を出す。

注釈オブジェクトのメニュー (`buildObjectMenu`) は種別ごとに項目が変わり、回転していない
矩形にだけ先頭へ「この範囲でトリミング」「この範囲を文字認識」「縦横比」が付く ―― 矩形は
図形であると同時に**画像に対する範囲**でもある、という位置づけ。見出しには切り出す寸法を
添えて選ぶ前に結果が分かるようにする(リサイズのプリセットと同じ考え方)。選択中は同じ寸法が
ステータスバー右側にも出る (`objectSizeText`) ―― 自前の数値入力ダイアログを持たないため、
これが範囲を合わせる唯一の数値表示になる。

縦横比 (`fitRectToAspect`) は範囲を整えるだけで**切り出さない**(比を決めてから位置を直せる
ように)。大きさは比の**整数倍**へ丸めるので、`16:9` と表示したものが画素数でも厳密に 16:9 に
なる ―― 丸めずに実数で合わせると、`cropRectFor` の floor/ceil で 1px ずれて比が崩れる。
整えた矩形は `ObjectMenuEntry::rect` に入れて App へ渡し、**見出しに出したものをそのまま**
`p1`/`p2` へ書く(整数座標なので `cropRectFor` を通しても同じ大きさに戻る)。同じ計算を
組み立て時と適用時の 2 回やると、見えていた寸法と結果が食い違いうるため。

手書き(ペン・マーカー = `Kind::Pen`)だけは選択領域ではなく**軌跡**が図形になる。
編集ドラッグ中の `onMouseMove` が通過点を `EditDragState` へ溜め(`appendPenPoint` が画面 2px
未満の動きを間引く)、`AnnotationSpec::points`(回転前の画像座標)として持つ。
`p1`/`p2` は点列の bbox に同期させ(`updatePenBounds`)、選択枠・ヒットテスト・回転中心・
ラスタライズ領域は他の種別と同じ経路を通す。移動・リサイズでは bbox と同じだけ点列も
動かす/拡縮する(`translateAnnotation` / `resizeAnnotation`)。ヒットテストは
**線の近傍だけ**で、bbox 内部は当たりにしない(囲むように描いた線の内側が丸ごと
掴めると、下の図形を選べなくなるため)。マーカーはペンと同じ種別で、線幅 4 倍・
不透明度 40%(`strokeAlpha`)という既定値の違いしかない(`EditStyle::applyTo`)。
連番マーカー (`Kind::Number`) は数字入りの円で、番号は `App::nextMarkerNumber` が
既存の注釈から数え直す(状態を持たないので undo・削除のあとも番号が詰まる)。
円を保つためドラッグは常に正方形へ寄せ、ハンドルも四隅だけにしてある。
現在のツールはステータスバー左側に表示する(モードが見えないと誤操作になるため)。

画像オブジェクト (`Kind::Image`) だけはツールを持たず、`Shift+V`
(`Command::PasteObject` → `App::executePasteObject`)でクリップボードから直接作る。
`AnnotationSpec::image`(`shared_ptr<const DecodedImage>`)に画素を持ち、p1/p2 の矩形へ
引き伸ばして描かれる。初期の位置と大きさは `pastedImageBounds`(等倍が基本。下地の
80% を超えるときだけ縦横比を保って縮め、可視領域の中心へ整数座標で置く)。
`shared_ptr` なので undo スナップショットへ何段積んでも画素は複製されない。
縦横比が崩れると見た目の事故になるため、ハンドルは四隅だけ・**既定でアスペクト維持で
Shift が解除**(`resizeKeepsAspect` が種別ごとに Shift の意味を決める)。焼き込みの
オーバーレイには上限があるので、貼り付けた時点で `downscaleToFit` で
`kMaxResizeDimension` まで縮めて取り込む(後から縮めても画素は戻らないため)。
下地が無いとき、および `IAnnotationRasterizer::available()` が false の環境
(SDL バックエンド。ライブ描画もラスタライズも未実装)では `Ctrl+V` と同じ
画像そのものの貼り付けへ回す ―― 見えず保存もされないオブジェクトを作らないため。

Shift ドラッグの寄せ方は種別で変える(`App::dragEndImage`)。矩形・楕円などは
選択領域を正方形にする (`constrainToSquare`) が、直線・矢印は bbox ではなく**線の向き**を
揃えたいので、始点から見て一番近い水平・垂直・45 度へ寄せる (`constrainToAxis`)。
どちらも移動量の小さいほうに合わせるので、画像の端まで引いても結果は画像内に収まる。
手書きも `constrainToAxis` だが、起点は始点ではなく **Shift を押した時点の点**
(`EditDragState::straightAnchorPoint`)。押している間はアンカーから先の
点列を捨てて直線 1 本で引き直し (`EditDragState::extendPen`)、離すとアンカーを捨ててまた
通過点を溜めるので、手書きと直線を 1 ストロークの中で混ぜられる。
`onShiftChanged` がドラッグ中の Shift の押し引きを拾い、マウスを止めたままでも
プレビューが追従する。

追加済みの注釈はパン役のボタンのクリックで選択して編集できる(掴んでいる間の状態は
`ObjectDragState`、ヒットテスト・回転・リサイズの幾何は core の `annotation_edit.cpp`、
いずれも純粋関数・単体テスト対象):
ドラッグで移動、四隅・辺のハンドルでサイズ変更(Shift で縦横比維持。回転中は
反対側のアンカーを固定。Line/Arrow は端点ドラッグで、Shift 中は固定端から見て
水平・垂直・45 度へ寄せる。Text は幅のみで高さは
ドラッグ確定時に実測へ正規化)、選択枠上の回転ハンドルで自由回転(Shift で 15° スナップ)、
右クリックでオブジェクトメニュー(回転角度プリセット・太さ・文字サイズ・フォント・
色・削除、手書きは線の不透明度、連番マーカーは番号。画像は自身の画素で描かれるため
削除と回転だけ)。
Text 注釈を選択中の `Ctrl+B` だけは `App::onKey` が Keymap より先に横取りし、
テキスト全体の太字を切り替える(`toggleSelectedTextBold`)。既定では
`Command::ToggleSidebar` と同じキーだが、選んでいるオブジェクトへの操作を優先する。
インプレース編集中の Ctrl+B/I/U が Keymap を通らないのと同じ扱いで、
選択 → 編集を行き来しても Ctrl+B の意味が変わらない。
矢印キーでの 1px 移動も「選んでいるオブジェクトを優先する」同じ規則だが、
こちらは直書きの例外ではなく Keymap の層として持つ(下記「選択中のキーバインド」)。
Ctrl+Z で1段階ずつ取り消し、Ctrl+Y(Shift+Ctrl+Z も可)でやり直せる。履歴は core の
`EditHistory` (`edit_history.h`) が持つ(1段は画像 + 注釈一覧のスナップショット
`EditSnapshot`、undo・redo とも上限 `EditHistory::kLimit` = 10)。undo は現在の状態を
redo 側へ、redo は undo 側へ積み替えるだけの対称な操作で、新しい編集を積む
(`EditHistory::push`)と redo 履歴は捨てる(分岐した未来は残さない)。
取り出したスナップショットを表示状態へ戻すのは App 側 (`App::restoreFrom`)。
ドラッグ(移動・回転・リサイズ)とテキスト入力は 1 回の操作で変更が何度も届くので、
積むのは最初の 1 回だけにする。その判定の旗も `EditHistory` が持つ
(`consumeDragPush` / `consumeTextEditSnapshot` / `consumeKeyMovePush`)。旗と積み先が
離れていると「積んだつもりで積んでいない」が起きるため。

保存は明示した時だけで、勝手に書き換えることはない。上書き保存 (Ctrl+S) は元の
ファイルを置き換えるため既定で確認ダイアログ (`IAppHost::showConfirm`) を出し
(`[save] confirm_overwrite`)、成功したら `ImageCache::invalidate` でその 1 件を捨てる
(捨てないと戻ってきたときに保存前のピクセルが出る)。名前を付けて保存は Shift+Ctrl+S。
**縮小して取り込まれた画像 (`DecodedImage::downscaled`) の上書きは断る**(確認ダイアログを
出す前に断る)。表示できる大きさへ縮めたピクセルで元ファイルを潰すと画素が戻らないため。
名前を付けて保存は許し、縮小した大きさをメッセージに出す。
**表示回転 (Viewport) は `current_` のピクセルには入っていない**ので、保存・コピー・
印刷・文字認識では `App::compositeImage` / `requestOcr` が注釈の後に回転を焼き込む
(画面で見えているとおりに外へ出す)。

印刷 (Ctrl+P、`App::executePrint`) も出口の一つで、保存と同じ `compositeImage` を
`IPrinter` へ渡す。紙は白なので、半透明を含む画像は文字認識と同じ理由(受け手が
アルファを見ない)で `flattenOnBackground` で白へ焼き込んでから渡す。用紙のどこに
どう置くかだけが `[print]` の設定で、プリンタ・用紙・向き・部数は OS の印刷ダイアログ
に任せる(自前の印刷設定 UI は持たない)。

### 印刷ダイアログとプレビュー (Windows)

Windows 版の `PrinterWin::print` は 2 つの経路を持ち、**モダン印刷ダイアログを試して、
駄目なら従来の `PrintDlg` へ落ちる**。

| 経路 | 実装 | プレビュー | 使われる場面 |
|---|---|---|---|
| モダン印刷 UI | `print_winrt.cpp` (WinRT `PrintManager` + Direct2D 1.1) | **出る** | 既定。Windows 8 以降で `PrintManager` を開ければこちら |
| 従来の印刷ダイアログ | `printer_win.cpp` (`PrintDlg` + GDI `StretchDIBits`) | 出ない | WinRT を開けない・D3D デバイスを作れない環境 |

**プレビューの中身は OS ではなくアプリが描く**。Windows 11 の印刷ダイアログは
プレビュー枠を持つが、ページを供給しないアプリには「このアプリは印刷プレビューを
サポートしていません」と表示されるだけで、`PrintDlg` にプレビューを出させる方法は無い
(ダイアログはこちらが 1 画素も描く前に開くので、OS には中身が分からない)。
埋めるには次の流れが要る:

1. `IPrintManagerInterop::GetForWindow` で HWND に紐づく `PrintManager` を取り、
   `PrintTaskRequested` を購読してから `ShowPrintUIForWindowAsync` でダイアログを出す
2. ハンドラで `CreatePrintTask` し、ソースとして `IPrintDocumentSource` +
   `IPrintDocumentPageSource` を実装したオブジェクト (`PrintDocument`) を渡す
3. OS が `GetPreviewPageCollection` → `Paginate`(`SetJobPageCount` で 1 ページと申告)
   → `MakePage` と呼んでくるので、**OS が用意した DXGI サーフェスへ D2D で用紙 1 枚を
   描き**、`IPrintPreviewDxgiPackageTarget::DrawPage` で返す
4. 「印刷」が押されると `MakeDocument` が来る。`ID2D1PrintControl` にコマンドリストを
   1 ページ追加して `Close` すれば、スプーラへ渡る

配置の計算は GDI 経路と同じ `layoutPrintImage` を使う(単位が DIP なので 1/100 DIP の
整数に直して渡す)。用紙の寸法・ハードウェア余白は `IPrintTaskOptionsCore::GetPageDescription`
から取り、`[print]` の余白と自動回転をそこへ適用するので、**プレビューに見えるものが
そのまま刷られる**。用紙の向きを変えると OS が `Paginate` から呼び直す。

実装上の注意:

- **プレビュー枠は 96dpi で合成される**。`MakePage` に渡る width / height (DIP) が
  そのままサーフェスのピクセル数で、`DrawPage` にも 96 を渡す。用紙はそこへ収まるよう
  D2D の変換で縮めて描く
- `DrawPage` の**前に `SetTarget(nullptr)` で描画先の割り当てを外す**こと。
  付けたままだと受け手が読めず、枠が「プレビューを読み込んでいます」のまま止まる
- 画面描画の `RendererD2D` は D2D 1.0 の `ID2D1HwndRenderTarget` なので流用できない。
  印刷は D2D 1.1 のデバイス・コマンドリスト・`ID2D1PrintControl` を要求するため、
  **印刷のときだけ** D3D11 + D2D 1.1 のデバイスを作って捨てる
- `d3d11.dll` は `LoadLibrary` で遅延解決し、WinRT も `combase` を遅延解決する
  (`winrt_abi.h`。OCR と共用)。**印刷を使わない起動では読み込まれない**
- WinRT インターフェースの実装に WRL の `RuntimeClass` は使わない。`InspectableClass`
  マクロが `windowsapp.lib` の静的リンクを要求するため、`IUnknown` / `IInspectable` は
  `print_winrt.cpp` の `ComObject` テンプレートで手書きする
- `ShowPrintUIForWindowAsync` の完了は「**ダイアログを出せた**」の合図であって、
  閉じた合図ではない。終わりは `PrintTask` の `Completed`
  (`Submitted` / `Canceled` / `Failed`)で受ける。それまで `IPrinter::print` は
  メッセージを回して待ち、その間だけ親ウィンドウを無効にする(従来の `PrintDlg` と
  同じモーダル感にするため)。回さずにブロックするとダイアログもプレビューも進まない

### 選択中のキーバインド (KeyScope)

「選んでいるオブジェクトへの操作を優先する」規則は、Ctrl+B のような直書きの例外では
なく **Keymap の第 2 レイヤー**として持つ。`Command` は `keyScopeOf` で
`KeyScope::Global` と `KeyScope::Selection` のどちらか一方に属し、App は表を 2 本持つ
(`keymap_` / `selectionKeymap_`)。`App::onKey` は**選択中だけ**
`selectionKeymap_` を先に引き、当たらなければ通常の表へ落とす。表を分けるのは、
`Keymap` が KeyChord → Command の 1 対 1 の map で、同じ `Right` に `NextImage` と
`MoveObjectRight` を同居させられないため。

既定は修飾なしの矢印 4 つ = `Command::MoveObject*`(画像座標で 1px 移動)だけ。
**Shift+矢印(ページ送り)・Ctrl+矢印(パン)・PageUp/PageDown(画像遷移)は
選択中もそのまま効く** ―― 同じキーの意味が選択の有無で変わらないほうが覚えやすく、
選択を解かずに画像も送れる。逆に修飾なしの矢印を渡すのは、取り違えたときの損害が
「1px 動く」側のほうが軽いため(未保存の編集そのものは下記の遷移ロックが守る)。
選択中は破線枠とハンドルが出ているので、どちらの意味になるかは画面から分かる
(「モードが見えないと誤操作になる」)。

向きは**画面基準**で、表示回転 (`R`) は `screenNudgeToImage`(純関数・単体テスト対象)が
打ち消す。移動量は画像 1px 固定でズーム率では変えない(トリミング枠の微調整が主用途で、
ハンドルでの 1px 調整と刻みを揃える)。画像の外へはみ出す位置も許す(マウスでの移動と同じ)。

**連続した移動は 1 段の undo にまとめる**(`EditHistory::consumeKeyMovePush`)。
1 打ごとに積むと上限 `kLimit` = 10 段をキーリピート数回で使い切り、それ以前の編集が
取り消せなくなるため。連なりの区切りは「移動以外の `Command` を実行したとき」
(`App::execute` の先頭)と「マウスで掴んだとき」(`beginObjectGrab`)の 2 か所で、
時計は使わない(core は時刻に依存しない)。

ini は `[keys]` の 1 セクションのままで、`applyConfig` に `KeyScope` を渡して
**自分の文脈に属さないコマンドの記述を読み飛ばす**(利用者は `move_object_left = A` と
書くだけでよく、表の区別を意識しなくてよい)。`F1` の一覧は「オブジェクト選択中」の
節を分けて出す(`buildHelpLines` は両方の表を受け取る)。

### 編集モードと遷移ロック

**未保存の編集がある間は画像の切り替えを断る**(`[edit] lock_navigation`、既定 true)。
画像を送れば `discardEdits` が注釈もトリミングも捨て、`EditHistory` ごと消えるので
`Ctrl+Z` でも戻せない ―― 高速なフォルダ内遷移が主用途である以上、うっかり 1 回
`→` を押すだけでその損失が起きる状態は割に合わない。

**モードは独立した状態としては持たない**。`App::editLocked()` は
`lockNavigation_ && ImageOrigin::edited()` の導出値で、専用のメンバは
`lockNavigation_`(設定値)だけ。これが設計上いちばん効いている点で、

- 編集モードへ入る仕組みを新しく書かなくてよい ―― `markEdited()`(オブジェクトの
  作成・移動・回転・リサイズ、テキスト入力、トリミング、画像リサイズ)が
  そのまま自動の入口になる
- 出る仕組みも同じく既存 ―― 上書き保存の `setEdited(false)` と、履歴を使い切った
  `Undo` の `setEdited(history_.canUndo())` で自然に解ける
- **「破棄するものが無いのにロックされる」状態が原理的に作れない**。閲覧しか
  していない利用者の操作は 1 つも変わらない(設計目標 2 を壊さないための条件)

そのため**手動でモードを切り替えるコマンドは用意しない**(用意すると上の最後の
性質が壊れる)。自動保存も入れない ―― 「保存は明示した時だけ」の原則に反するため。

断り方は頻度で二段に分ける。**押した回数だけモーダルが出る操作を作らない**のが要点:

| 経路 | 断り方 | 実装 |
|---|---|---|
| 矢印・ホイール・サイドボタン・オーバーレイ矢印・`Home`/`End` | ステータスバーの通知 | `App::guardEditLock`(`Command::NextImage` などの `case` の中。`list_.next()` は位置を書き換えるので**判定を先に**) |
| サイドバーの項目クリック | 同上 | `App::clickSidebarItem`(`jumpTo` の前) |
| ページ送り (`Shift+←/→`) | 同上 | `App::stepFrame`。フレーム切替も編集を捨てるので同じ扱い |
| 貼り付け (`Ctrl+V`) | 同上 | `App::executePasteImage`。注釈として貼る `PasteObject` は編集そのものなので断らない |
| ファイルを開く (`Ctrl+O`・D&D) | 確認ダイアログ | `App::confirmEditLock`(`App::openPath` の先頭)。一度きりの明示的な操作で、黙って無反応にすると故障に見える |

通知は**抜け方を名指しする**(`App::editLockHint`)。`編集中は画像を切り替えられません
(Ctrl+S 保存 / Esc 破棄 / Ctrl+Z 取り消し)` の形で、キーは `Keymap` から引くので
ini で変えた割り当てがそのまま出る。ヘルプを見に行かせないための唯一の案内。

ロックを解く 3 つ目の道である**破棄は `Command::DiscardEdits` として新設**した。
既定のキーは持たず、**`Esc` の連鎖に 1 段挟んである** ―― `Esc` は元から
「一段ずつ解除していく」意味なので置き場所として自然で、同時に
「編集中に `Esc` を押すと未保存のまま即終了する」という穴も塞がる。連鎖の順は
テキスト編集の終了 → 選択解除 → 編集ドラッグの中止 → **編集の破棄** →
ヘルプを閉じる → フルスクリーン解除 → 終了。破棄は undo でも戻せないので、必ず
`IAppHost::showConfirm` で確認する。**この連鎖は `lock_navigation` を切っても効く**
―― 設定が受け持つのは遷移を断るかどうかだけで、「未保存のまま `Esc` で終了させない」
のは別の安全策(判定も `editLocked()` ではなく `ImageOrigin::edited()` を見る)。

破棄は注釈を消すだけでは足りない(トリミング・リサイズしたピクセルが残ってしまう)。
**読み直せるならファイルから読み直す** (`refreshCurrent`) ―― 画像を送って戻ってきた
ときとまったく同じ状態になる。貼り付け画像には読み直す元が無いので、そのときだけ
`EditHistory` を遡れるところまで遡って戻す。

見せ方は既存の 3 か所だけで、新しい描画要素は足していない
(「モードが見えないと誤操作になる」):

1. タイトルバーの `(編集中)`(`windowTitle`)
2. ステータスバーの `編集中(遷移ロック)`(`statusText`。`lock_navigation = false`
   のときは出さない ―― 出しても操作は何も変わらないため)
3. **オーバーレイ矢印を出さない**(`App::navArrowsGeometry`)。押しても遷移できない
   ボタンを見せない

**既知の穴: 終了はロックしていない。** `Q` / `Ctrl+W` / タイトルバーの `✕` は
未保存の編集を確認なしに捨てる(`Esc` だけは上の連鎖で破棄を訊く)。`✕` は
`WM_CLOSE` なので `IAppHost` 側の経路が要り、core だけでは閉じない ―― 3 つの
入口で挙動が食い違わないよう、まとめて別途対応する。

### アニメーションと多フレーム画像

アニメーション GIF・多ページ TIFF・ICO の複数サイズは「**1 ファイルが N 枚のフレームを持つ**」
という 1 つの概念にまとめてある(`ImageSequence`)。違うのは**勝手に進むかどうか**だけで、
`SequenceKind` で振る舞いを分ける。

| 種別 | 対象 | フレーム間の依存 | 実装方針 |
|---|---|---|---|
| `Single` | 通常の画像 | — | 従来どおり(1 フレームの列として扱う) |
| `Pages` | 多ページ TIFF、ICO の各サイズ | **独立**(単独でデコードできる) | 必要になったページだけ遅延デコード |
| `Animation` | アニメーション GIF | **依存**(前フレームの合成結果が要る) | 全フレームを裏でまとめて展開 |

この「依存の有無」が最大の設計軸。多ページ TIFF は 300dpi A4 が 50 ページで展開後 1.6GB に
なりうるので全ページ保持はあり得ず、逆に GIF は前フレーム無しにフレーム N を作れないので
「全部持つ」か「毎回頭から再生し直す」かの二択になる(後者はデコードワーカー 1 本を
ループのたびに占有して先読みを止めるので採らない)。

**`DecodedImage` は 1 フレームのままにしてある。** `App::current_` は常に「現在のフレーム」を
指すので、保存・コピー・印刷・文字認識・注釈・レンダラはこの機能を一切意識しない。

- **`frames[0]` は必ず `IImageDecoder::decode` が返すものと同じ絵**にする。並び順はデコーダが
  決めてよく、**ICO は大きい順に並べ替える**(ファイル内の先頭は 16x16 のことが多く、そのまま
  出すと「アイコンを開いたのに小さすぎる」ことになる)。App から見た index は常にこの並び
- **`ImageCache` のエントリはファイル単位のまま**で、値が `ImageSequence` になった。
  キーを `(パス, フレーム番号)` にする案は、`maxItems`(既定 8 枚)が「100 フレームの GIF =
  100 エントリ」で意味を失うので採らなかった。バイト数は存在するフレームの合計で数える
- **フレームが増えるたびに `ImageSequence` を作り直して差し替える**(コピーオンライト)。
  UI スレッドが持ち出した `shared_ptr` の中身は変わらないので、ロックなしで読める
  (`adoptRefinedImage` と同じ考え方。App は完了通知のたびに `adoptSequence` で取り直す)
- **フレーム構成の調査 (`probeSequence`) は表示に採用した時点で予約する**(先読みでは行わない)。
  さらに `mayHaveMultipleFrames`(拡張子が `.gif` / `.tif` / `.tiff` / `.ico`)で絞る ——
  すべてのファイルで調べると写真 1 枚を開くたびにファイルを二度開くことになり、設計目標 #1 に反する
- ワーカーの優先順位は `urgent_ → probe_ → prefetch_ → page_ → animation_ → refine_`。
  調査は安くて案内(ページ数)を待たせるので先読みより上、全フレーム展開は前後への移動の
  軽さを優先して先読みより下

**APNG とアニメーション WebP は非対応**。WIC の PNG / WebP デコーダがフレームを列挙しないため、
対応するには core に自前のデマルチプレクサ(`fcTL`/`fdAT` の解析、RIFF の `ANMF` の解析)を
書くことになる。`.png` / `.webp` を `kMultiFrameExtensions` に入れていないのはこのため。

再生まわり:

- 合成(部分矩形の配置・Disposal・Blend)と遅延時間の正規化、再生位置の前進は
  **core の `animation.h`**(純粋関数・単体テスト対象)。WIC/stb 層はメタデータを読んで渡すだけ
- **GIF の Background は透明で塗る**(背景色ではない。主要ブラウザと同じ挙動)
- 遅延 0 / 10ms の指定は既定値(100ms)へ読み替える。素の値に従うと数百 fps になる
- **`IAppHost::setFrameTimer` は `startTimer` とは別のタイマーにすること**。あちらは
  ステータスバーの通知を消すための単発タイマーで、実装は同じ ID を張り直して満了時に止める。
  共用すると再生中に通知が出た瞬間にアニメーションが止まる
- 上限(`[animation] max_memory_mb` / `max_frames`)を超えるアニメーションは**静止画として開く**。
  途中まで再生すると不具合にしか見えないので、中途半端には持たない

操作と規則:

- `Space` = 再生 / 一時停止(`TogglePlay`)。**フレーム送りは画像遷移と完全に分けてある**
  (`Shift+←` / `Shift+→`)。端では折り返さず、最終ページの次で次のファイルへも進まない
  —— 折り返すと「末尾に着いた」ことが分からず、ファイルへ進むとタイトルの `[i/n]` と
  サイドバーの整合が崩れるため
- **編集を始めたら再生は止まる**(`pushUndoState` が `stopPlayback` を呼ぶ)。以後は
  「そのフレームの静止画」を触っていることになる
- **フレーム切替は画像切替と同じ扱い**(新しい規則を増やさない)。編集は破棄され、
  未保存の編集がある間は遷移ロックが送り自体を断る
- **フレームが 2 枚以上ある画像の上書き保存は断る**。表示中の 1 枚で潰すと残りが消えるため、
  「縮小して取り込んだ画像は上書きしない」と同じ扱いで確認ダイアログの前に断る
- ズーム・パンはフレーム間で保つ。**大きさが変わるページ(ICO のサイズ違いなど)だけ
  フィットし直す**(アニメーションは全フレームが同じ論理画面なので影響を受けない)
- フレーム番号は**ステータスバー**に出す(タイトルの `[i/n]` はフォルダ内の位置なので、
  括弧を 2 つ並べると読めなくなる)。サイドバーはファイル一覧のままで、ページは並べない

SDL 版は **GIF だけ**対応する(`stbi_load_gif_from_memory` が Disposal 込みで全フレームを返す)。
多ページ TIFF・ICO は stb_image の範囲外。stb は全フレームを 1 回の確保で返すため、
**上限の判定が展開の後になる**のは SDL 版の既知の制限。

### 一覧の並び順とサブフォルダ

フォルダ内の画像を **どう集めるか** は platform、**どう並べるか** は core という分担にしてある。

- `IFileSystem::listImages` は `FileEntry`(パス・起点からの相対パス・更新時刻・サイズ)を
  返し、**必ず「親フォルダの相対パス → ファイル名」の 2 段の自然順(昇順)**に並べる。
  比較子は Win が `StrCmpLogicalW`、SDL が `naturalCompare`(どちらもエクスプローラ準拠)。
  相対パスを 1 本の文字列として比較しないこと(区切りの `\` が英数字より後ろに来るため
  `a\b.jpg` と `a.jpg` の並びが直感に反する)
- core の `sortedOrder`(`sort_order.h`)はこの**名前昇順の入力**を前提に、主キーだけを見る
  `std::stable_sort` で表示順(添字の並び)を作る。同値の順序が名前昇順のまま残るのは
  この契約のおかげ。**降順は `std::reverse` ではなく比較子の反転**で実装している
  (`reverse` だと同値内の名前順まで裏返る)
- `App` は列挙結果 `entries_` を名前昇順のまま保持し、表示順は `order_`(`entries_` への
  添字)で表す。並び替えは `entries_` を触らず `order_` を作り直すだけ

`FileEntry::lastWriteTick` は**大小比較にしか使わない値**で、`time_since_epoch().count()` を
そのまま入れてある(`file_time_type` → Unix 時刻の変換は処理系差が出るが、並び替えに要るのは
順序だけ)。日時を表示したくなったら platform 側に整形を頼むこと。

並び順を変えても**表示中の画像は変わらない**(番号だけが変わり、再デコードも起きない)。
`ImageList::set` がパス一致で位置を復元するため。`App::relist` は、表示中の画像が一覧から
消えたとき(再帰を切ったなど)だけ `refreshCurrent` を通す — 残っているのに通すと
編集中の内容を捨ててしまうため。

**サブフォルダの再帰は 2 段階で読む**。`App::openPath` はまず直下だけを同期列挙して
表示を確定し(現状と同じ速度)、再帰が有効なときだけ `ScanService` へ全体の走査を回して、
完了したら一覧を差し替える。ツリー全体を UI スレッドで歩くと最初の 1 枚が出るまで
固まり、設計目標 #1(高速起動)に反するため。再帰を**切る**ほうは直下の同期列挙で
足りるのでワーカーを起こさない。走査にはガードが 3 つある:
シンボリックリンク(ジャンクション)を辿らない(循環で無限走査になる)、
隠しフォルダを辿らない(`.git` のような大量のファイルを踏まない)、
`App::kMaxListFiles`(10 万件)で打ち切る。

再帰中はサイドバーに**起点からの相対パス**を出す(ファイル名だけでは別フォルダの
同名・連番が区別できない)。ステータスバーの「サブフォルダ含む」は常時表示で、
これが無いと隣のフォルダの画像が出てくる理由が分からなくなる。

並び替えと再帰のコマンド(`sort_*` / `recursive`)は**既定のキーを持たない**。
入口は**サイドバー(ファイル名一覧)の右クリックメニュー**で、エクスプローラと同じ場所なので
説明が要らない(操作一覧モードでは並べ替える一覧が無いので出さない)。

### マウス操作への Command の割り当て

**左右ボタン以外のマウス操作は `Mousemap` で Command に解決する**(`MouseChord` →
`Command`。`Keymap` と同じ形で、既定表 + ini の `[mouse]` 上書き、逆引きも持つ)。
対象は中ボタン・サイドボタン (X1/X2)・垂直/水平ホイール・左ダブルクリック
(`MouseInput`)で、左右ボタンのクリック・ドラッグは**含めない**。パン・編集ドラッグ・
メニューで埋まっており、割り当ての対象にすると次節の `mouseRole`(`swap_buttons`)と
二重管理になるため。

| 既定の割り当て | Command |
|---|---|
| `X1` / `X2`(サイドボタンの戻る / 進む) | `PrevImage` / `NextImage` |
| `Ctrl+WheelUp` / `Ctrl+WheelDown` | `PrevImage` / `NextImage` |
| `WheelLeft` / `WheelRight`(水平ホイール) | `PrevImage` / `NextImage` |
| `Middle` / `DoubleClick` | なし(ini で割り当て可) |

**ホイールでのズームは Mousemap に載せない**。カーソル位置を基準に拡大縮小するため
`Command::ZoomIn` と等価にならず、「**割り当てのないホイールの既定動作**」として
`App::onWheel` が受け持つ。この作りにすると `[mouse] next = WheelDown` と書くだけで
素のホイールが遷移になり、既定の `Ctrl+ホイール` の割り当ては消えてズームへ落ちるので、
`wheel = zoom|navigate` のようなモード設定が要らない。サイドバー上のホイールだけは
割り当てより一覧のスクロールを優先し(修飾キー付きも同じ)、水平ホイールに至っては
何もしない(横スクロールする中身が無く、一覧を読んでいる最中に画像が切り替わると
邪魔になるため)。

遷移は離散なので、ホイールの回転量は `core` の `consumeWheelSteps` で 1 段に
達するまで貯めてから消費する(1 ノッチ未満ずつ通知される高精細ホイール・
タッチパッドで取りこぼさないため。逆向きに回したら貯金は捨てる)。垂直と水平で
別の貯金を持ち、どちらも `PointerState`(`PointerState::wheelSteps`)が抱えている。

**水平ホイールは誤爆しやすい軸**なので、貯め方に 2 つ手当てがある。トラックボールや
タッチパッドは縦スクロール中に微小な横成分を出し続け、素朴に貯めると必ず 1 段に
達して画像が勝手に切り替わるため:

- 垂直ホイールのイベントが来たら水平の貯金は捨てる(軸ロック。逆はしない。
  縦を優先する非対称な扱いは意図したもの)
- それでも足りなければ、1 段とみなすノッチ数を `[mouse] wheel_horizontal_threshold`
  (既定 1 = 垂直と同じ、1-10)で鈍くできる

ウィンドウ層は `MouseChord` を組み立てて `App::onMouseInput` を呼ぶだけ
(キー入力が `KeyChord` を組み立てるのと同じ)。win 層の注意点として、
**`WM_XBUTTONUP` を `DefWindowProc` へ渡してはならない**(`WM_APPCOMMAND` の
BROWSER_BACKWARD/FORWARD が生成され、サイドボタンが二重に届く)。X ボタンの
メッセージは押下・解放とも消費して `TRUE` を返す。

ダブルクリックは**テキスト注釈の再編集が優先**で、そこに当たらなかったときだけ
`MouseInput::DoubleClick` の割り当てを見る(`App::onDoubleClick`)。

### オーバーレイ矢印(画像遷移ボタン)

ポインタがビューポート左右の端の帯(`kNavArrowBandPx`)に入ると、前後の画像へ移る
ボタン(◀ ▶)を画像の上に重ねて出す。**クリックが効くのはボタンの内側だけ**で、
帯全体を当たりにはしない(端をクリックしたつもりで画像が変わるのを防ぐ)。
先頭・末尾では行き先の無い側を出さない。`[view] nav_arrows = false` で完全に消せる。

出さない条件は `App::navArrowsGeometry` に集約してある: 設定で無効、フォルダが空、
ポインタがウィンドウ外(`onMouseLeave`)かビューポート外、ドラッグ中(パン・編集・
オブジェクト・サイドバー幅)、テキスト編集中(端のボタンを押して編集が消えるのを防ぐ)。

クリックは**サイドバーの項目と同じ UI 部品**の扱いで、`swap_buttons` にかかわらず
常に左ボタン。`onMouseDown` では注釈を掴む判定より先に見る(端に図形があっても
ボタンが押せるように)。ただし SDL 版で `swap_buttons = true` にすると左ボタンが
編集役になり、`WindowSdl` が編集役のボタンを App へ渡さないためボタンを押せない
(SDL 版は閲覧専用で、入れ替える動機自体が無いので放置してある)。

**この表示は使用感が合わなければ廃止しうる**前提で、次の 6 か所に閉じ込めてある
(消すときはこれだけを消せばよく、既存の入力・描画の流れには手を入れていない):

1. `core/nav_arrows.h` / `.cpp` — 寸法と表示・当たりの判定(純粋関数、単体テスト対象)
2. `platform/renderer.h` の `NavArrowsView` と `render` の引数
3. `App::navArrows` / `navArrowsGeometry` / `clickNavArrow` / `updateNavArrowHover` と
   `navArrowsEnabled_` / `navArrowsShown_`(ポインタが窓内にいるかは
   `PointerState::inside()`。こちらは矢印専用ではないので残る)
4. `RendererD2D::drawNavArrows`(+ 山形用の `navGlyphStroke_`)
5. `RendererSdl::drawNavArrows`
6. `[view] nav_arrows` の読み取り

再描画は `onMouseMove` / `onMouseLeave` が `updateNavArrowHover` で「表示・ホバーが
変わったときだけ」要求する(ステータスバーのホバー表示と同じ考え方。ポインタが動く
たびに再描画しない)。`navArrowsShown_` はそのための直前の状態で、描画の正は
毎回組み立てる `navArrows()` のほう。

操作一覧 (F1) には載せていない。ボタン自体が「マウスで遷移できる」ことの案内なので、
一覧に書いても見る人はもう知っている(廃止時に消す箇所を増やしたくないのもある)。

### マウスボタンの役割

物理ボタン (`MouseButton`) と役割 (`MouseRole`) を分けてあり、対応は
blinker.ini の `[mouse] swap_buttons` で入れ替えられる(`PointerState::role`。
`App::mouseRole` はその転送で、ウィンドウ層と操作一覧はこちらを見る)。
こちらは左右ボタンだけの話で、上記の Mousemap とは独立している
(`[mouse]` セクションを共有するが、`swap_buttons` はコマンド名として解決されない
ので `Mousemap::applyConfig` は無視する)。

| 役割 | 既定 | 入れ替え時 | 内容 |
|---|---|---|---|
| `Pan` | 左 | 右 | 何も掴まなかったドラッグで画像をパンする |
| `Edit` | 右 | 左 | 何も掴まなかったドラッグで現在のツールを実行する |
| オブジェクト | 左 | 左 | 注釈の選択・移動・回転・サイズ変更、キャレット移動(**入れ替えない**) |
| メニュー | 右 | 右 | ツール切り替え・オブジェクト・書式メニュー(**入れ替えない**) |

入れ替えの対象が「パン」と「編集ドラッグ」だけなのは、メニューを開くボタンが
状況で変わると押せなくなるため。右ボタンは常にメニュー役も兼ね、ドラッグ量が
`PointerState::kDragThresholdPx` 未満のまま離されたときだけ `showPointerMenu` が開く
(既定ではその右ボタンが編集役でもあるので、編集ドラッグを始めていても
閾値未満なら何も作らずメニューになる)。押した場所を覚えてこれを判定するのは
`PointerState`(`pressMenu` → `releaseMenu` が `MenuOnRelease` で「どのメニューを
開くか」を返す)で、App はその結果でメニューを出し分けるだけ。サイドバーの項目
クリックは画像への操作ではないので、これも入れ替えず常に左ボタン。

サイドバーの幅は右端(境界をまたぐ `SidebarState::kResizeGripPx` の帯)を左ボタンで掴んで
変えられる。`onMouseDown` はこの判定を項目クリックより先に見る(境界際のクリックで
画像が切り替わってしまわないように)。掴んだ位置からの総移動量で幅を決めるので
(`SidebarState::resizeWidth`)、下限・上限に当たってポインタが端から離れても戻せば追従する。
下限はモードごとに `SidebarState::width()` が返す幅と揃える(操作一覧は `kHelpWidth`)。
上限は `kMaxWidth` と「窓幅 - `kMinViewportWidth`」の狭いほう。ini の
`sidebar_width` は起動時の幅で、ドラッグでの変更は保存しない。

サイドバーの項目は**掴んで他のアプリへ落とせる**(エクスプローラへのコピー、
他のアプリへの受け渡し)。押下は従来どおり `clickSidebarItem` でその画像へ移動し、
`pressSidebarItem` が掴んだパスを控えるだけ。`kDragThresholdPx` を超えて動いた時点で
`beginSidebarFileDrag` が `IAppHost::beginFileDrag` を呼ぶ ―― **控えるのが index ではなく
パス**なのは、押下から移動までの間に並び替えやサブフォルダ走査の完了で一覧が
入れ替わりうるため。win 層は OLE の `DoDragDrop`(`win/drag_drop_win.cpp`)で、
`SHCreateDataObject` の空のデータオブジェクトへ `CF_HDROP` を積むだけなので
`IDataObject` の自前実装は要らない。落とし先へ許すのは**コピーとリンクだけ**で、
移動は許さない(閲覧しているファイルがドラッグひとつで消えるのを防ぐ)。
そのため `wWinMain` の COM 初期化は `CoInitializeEx` ではなく `OleInitialize`
(`DoDragDrop` が OLE の初期化を要求する)。`beginFileDrag` は落とされるまで返らず、
`DoDragDrop` が自分でマウスを捕捉するので、**呼ぶ前にウィンドウのキャプチャを手放し、
対になる `WM_LBUTTONUP` が来ない前提で押下状態を畳んでおく**。SDL 版はドラッグ元に
なれる API が無いため何もしない。

オブジェクトを掴む操作も入れ替えない。既存の図形を選ぶのに右クリックが要るのは
他のペイント系ソフトと食い違って戸惑うため、左ボタンのままにしてある。そのため
`onMouseDown` は**左ボタンだけ**まず `beginObjectGrab` を通し、掴めなかったときに
初めて `mouseRole` どおりの動き(パン / 編集ドラッグ)へ落とす。役割は「掴めなかった
ときに何をするか」を決めるだけ、という位置づけ。解放も対称で、`endObjectGrab` は
左ボタンのときだけ呼ぶ(入れ替え時の左は `endEditDrag` も通るが、掴んでいたなら
編集ドラッグを始めていない (`EditDragState::dragging` が false) ので素通りする)。

この結果、入れ替え時は既存の図形の上から新しい図形を描き始められない(左が選択に
なるため)。図形の無いところから描き始めればよいので、選択できないより実害が小さい
という判断。

振り分けは core に閉じている。ウィンドウ層はボタンの押下・解放・移動をそのまま
App へ渡すだけで、パンの差分計算(`PointerState::moveTo`)もドラッグ中かの判定も
core が持つ(役割が設定で変わるので、ウィンドウ層に状態を置くと二重管理になる)。
SDL 版だけは編集役のボタンを App へ渡さない(注釈編集が未対応のため)。

### テキストのインプレース編集

Text 注釈は PowerPoint のテキストボックスと同じく**画像上で直接編集する**
(モーダルなテキスト入力ダイアログは持たない)。テキストツールで編集ドラッグすると
ドラッグした矩形が空のテキストボックスになって編集が始まり、既存のテキストは
ダブルクリック(またはオブジェクトメニューの「テキストを編集」)で編集に入る。

- 文字列・キャレット・選択範囲は core の `TextEditBuffer`(`text_edit.h`、UTF-8 の
  バイト位置で持つ純粋ロジック、単体テスト対象)。その周りの編集セッションの状態
  (編集中か・どの注釈か・新規作成中か・キャレットの点滅相・ドラッグ選択中か・
  書式メニューの押下・IME の変換中文字列)は `TextEditState`(`text_edit_state.h`)が
  抱え、`App` は編集の開始と終了を `begin` / `end` で伝える。
- 折り返し位置に依存する操作(クリックでのキャレット移動、上下キー、キャレットと
  選択範囲の描画位置)は `IAnnotationRasterizer` のテキスト計測
  (`caretMetrics` / `hitTestTextOffset` / `selectionRects`、Windows は DirectWrite)
  に委ねる。位置指定は UTF-16 コード単位で、境界の変換は `core/unicode.h` の
  `utf8ToUtf16Offset` / `utf16ToUtf8Offset`。
- 編集中は `App::onKey` がキー入力をコマンドではなく編集操作へ回す(未対応キーも
  握りつぶして移動コマンドの暴発を防ぐ)。文字そのものは win 層の `WM_CHAR` →
  `App::insertText` で入る。
- 枠の幅は編集中は固定し(入力のたびに折り返しが変わるのを防ぐ)、高さだけ内容に
  追従させる。確定時に `measureTextExtent` で幅も内容へ詰める。
- 確定(Esc・枠外クリック・編集ドラッグの開始・画像切替)の時点で内容が空なら
  テキストボックスは残さず削除する。undo は編集開始前のスナップショットを
  最初の変更時に 1 回だけ積む。
- `IAppHost::setTextEditing` で編集の開始・終了とキャレット位置を win 層へ伝える。
  win 層はこれで **編集中だけ IME を有効化**し(通常時は `n` などがコマンドのため
  `ImmAssociateContextEx` で切ってある)、変換ウィンドウをキャレット位置へ寄せ、
  キャレット点滅タイマーを回す。
- **IME の変換中文字列はテキストボックス内へインライン表示する**。win 層は
  `WM_IME_STARTCOMPOSITION` / `WM_IME_COMPOSITION` を消費して既定の変換ウィンドウを
  抑止し、`GCS_COMPSTR`(文字列)・`GCS_CURSORPOS`(キャレット)・`GCS_COMPATTR`
  (変換対象の節)を `App::setComposition` へ渡す。確定文字列 (`GCS_RESULTSTR`) は
  `App::insertText` へ。候補ウィンドウは `ImmSetCandidateWindow` (CFS_EXCLUDE) で
  キャレット行を避けさせる。
- 変換中文字列は **`TextEditBuffer` には入れず**、`TextEditState::displayText` が
  キャレット位置へ挿入した形で描画にだけ混ぜる(確定するまで編集内容にしない、
  undo にも積まない)。下線は変換中の範囲を細線、変換対象の節を太線で引く
  (範囲の矩形は選択範囲と同じ `selectionRects` で求める)。変換中はキー入力・
  クリックによるキャレット移動を App 側で握りつぶし、IME に任せる。
- 編集中のテキストボックスの内側ではマウスカーソルを I ビームにする
  (`App::wantsTextCursor` を win 層が `WM_SETCURSOR` で参照する)。編集の開始・終了は
  マウスが動かず `WM_SETCURSOR` が届かないため、`setTextEditing` から送り直す。
  サイドバーの右端では同じ仕組みで左右の矢印にする(`App::wantsSidebarResizeCursor`。
  SDL 版は `WindowSdl::updateCursor` が `SDL_EVENT_MOUSE_MOTION` で切り替える)。

### 部分書式(選択文字列の色・太字・斜体・下線・フォント)

同じテキストボックスの中で、選択した範囲だけに色・太字・斜体・下線・フォントを付けられる。

- 書式は `AnnotationSpec::styles`(`TextStyleRun` の配列)として、文字ではなく
  **UTF-8 バイト範囲**に対して持つ。範囲リストは core の `text_style.h` が
  「昇順・重なりなし・既定のままの範囲は持たない」正規形に保つ(単体テスト対象)。
  覆われていない部分は注釈全体の `colorRGB` / `fontFamily` と標準の太さで描かれる。
- 編集中の書式は `TextEditBuffer` が文字列と一緒に持ち、挿入・削除のたびに
  `adjustTextStyles` で位置を追従させる。挿入位置で終わる範囲が挿入分を取り込むため、
  **入力した文字は直前の文字の書式を継ぐ**(一般的なエディタと同じ)。変換中文字列を
  混ぜた描画用の書式(`TextEditState::displayStyles`)も同じ規則でずらす。
- 操作は編集中の Ctrl+B(太字)・Ctrl+I(斜体)・Ctrl+U(下線)と、選択範囲の上での
  右クリックで出る書式メニュー(太字・斜体・下線・フォント・文字色…)。右クリックは
  通常なら編集を確定するが、選択範囲の上でだけは確定せずメニューを出す。
  フォントの候補と既定の解決はツールメニューと同じ(`fontFamilyChoices`)。
- 描画は Windows のみ。太字・斜体・下線・フォントは文字送りと行高に影響するため
  `createAnnotationTextLayout` がレイアウト生成時に適用し(キャレット位置・
  ヒットテストの計測も同じレイアウトを通るので表示と一致する)、色は見た目だけなので
  描画時に `applyTextColorEffects` が `IDWriteTextLayout::SetDrawingEffect` へ
  ブラシを載せる(D2D の既定テキストレンダラがその範囲だけ別ブラシで描く)。
  ブラシはデバイス依存なので、描画に使うレンダーターゲットで作ること。

## 主要コンポーネント

| コンポーネント | 責務 |
|---|---|
| `App` | 状態機械の中心。Command を受けて状態更新、host へ再描画依頼。ステータスバー (`StatusBarView`) とサイドバー (`SidebarView`、可視範囲の項目のみ) の表示内容もここで組み立てる(文字列そのものは `view_text.h` の純関数が作る)。サイドバーの表示状態は `SidebarState` が持ち、`SidebarMode` でファイル名一覧と操作一覧 (F1) を切り替える(レンダラからは同じ文字列リストに見える)。マウスの進行状態は `PointerState`、編集ドラッグは `EditDragState`、注釈オブジェクトを掴んでいる間は `ObjectDragState`、テキストのインプレース編集の進行状態は `TextEditState`、選択中のツールと新規注釈の見た目は `EditStyle` が持つ。表示中の画像がどこから来たか(どのファイルか・貼り付け画像か・読み込みに失敗したか)と未保存の編集の有無は `ImageOrigin` が持つ。貼り付け画像はフォルダ一覧から独立した表示状態(`ImageOrigin::fromClipboard`)で、移動系コマンドで一覧表示へ戻る(一覧が空なら戻る先が無いので移動を無視する。`App::navigate`)。編集(現在のツール `EditTool` と編集ドラッグでの適用、プレビュー・`SelectionView`・注釈オブジェクトの選択/移動/回転ドラッグ状態・undo 履歴 (`EditHistory`))もここで管理し、画像切替で破棄する。ただし未保存の編集がある間は切り替え自体を断る(`App::editLocked`。「編集モードと遷移ロック」の節) |
| `Viewport` | ズーム/パン/フィット/回転の座標変換(純粋計算、テスト容易) |
| `ImageList` | フォルダ内画像の一覧・現在位置・先読み候補の順序付け |
| `ImageOrigin` | 表示中の画像の出どころ(`image_origin.h`。純粋な状態、単体テスト対象)。読み込み元のパス・貼り付け画像かどうか・読み込みに失敗したかとその理由・未保存の編集があるか。状態を変える口は `setFile` / `setFailed` / `setLoading` / `setClipboard` / `clear` の 5 つだけで、失敗の理由や貼り付けの印を消し忘れて次の画像に持ち越すことがないようにしてある。**画素も一覧も窓も知らない** ― 画素 (`App::current_`) との対応付けと、一覧の現在位置との突き合わせ(`ImageOrigin::path()` と `ImageList::current()` のずれが「読み込み中」)は `App` に残る |
| `sort_order.h` | 一覧の並び順(名前・更新日時・サイズ・種類 × 昇降)の適用。**列挙とプラットフォーム依存の名前順比較は `IFileSystem` の責務**で、ここは「どのキーでどちら向きに並べるか」だけを持つ純粋関数(単体テスト対象)。詳細は下記「一覧の並び順とサブフォルダ」 |
| `ScanService` | ワーカースレッド1本でサブフォルダを再帰列挙(`OcrService` と同じ形)。予約は最後の1件だけが走り、結果は generation 付きで返る |
| `ImageCache` | ワーカースレッド1本で非同期デコード。**エントリはファイル 1 つで、値は `ImageSequence`(1 枚以上のフレーム)**。LRU で、枚数とバイト数の**両方**を上限にする(既定: 8枚 / 512MB。`[cache] max_items` / `max_memory_mb` で変更でき、読み取りは `cacheLimitsFromConfig`)。表示中の可能性が高い直近の1枚は上限を超えても捨てない。仕事の優先順位は 表示 (`urgent_`) → **フレーム構成の調査 (`probe_`)** → 先読み (`prefetch_`) → **ページのデコード (`page_`)** → **アニメーションの全フレーム展開 (`animation_`)** → **色変換の読み直し (`refine_`)** の順。色変換は `DecodedImage::colorPending` が立った画像を `decodeColorManaged` で読み直して差し替える(1 枚につき一度だけ試し、成功したときだけ完了通知を出す)。フレームが増えるときは `ImageSequence` を作り直して差し替える(コピーオンライト)ので、UI スレッドはロックなしで読める |
| `animation.h` | アニメーションの合成(`AnimationCompositor`。部分矩形の配置・Disposal・Blend)、遅延時間の正規化(`normalizedDelayMs`)、再生位置の前進(`advanceFrame`)。いずれも純粋関数・単体テスト対象で、時計にも OS にも依存しない。設定(`[animation]`)の読み取りもここ |
| `OcrService` | ワーカースレッド1本で非同期に文字認識(`ImageCache` と同じ形)。予約は最後の1件だけが走り、結果は generation 付きで返るので、画像を切り替えた後に届いた古い結果を App 側で捨てられる |
| `ocr_text.h` | 認識結果の後処理。テキストの整形(`ocrResultToText`。行の連結と、CJK 文字に挟まれた空白の除去 — Windows の OCR は日本語でも語間に空白を入れて返す)と、拡大して読み直すかの判断(`ocrRetryUpscale`)。閾値は実測で決めてあり、根拠はヘッダのコメントに残してある。純粋関数で単体テスト対象 |
| `Keymap` | KeyChord → Command。デフォルト表 + ini 上書き。逆引き (`chordsFor` / `chordToString`) も持ち、操作一覧の生成に使う。表は文脈 (`KeyScope`) ごとに別インスタンスで、`defaults` が通常、`selectionDefaults` がオブジェクト選択中(上記「選択中のキーバインド」) |
| `Mousemap` | MouseChord → Command(中・サイドボタン・ホイール・ダブルクリック)。`Keymap` と同じ形。ini 用の表記 (`chordToString`) と操作一覧用の日本語表記 (`chordToDisplayString`) を持つ。ホイール量の蓄積 (`consumeWheelSteps`) も同じヘッダ(いずれも単体テスト対象) |
| `nav_arrows.h` | オーバーレイ矢印(左右の端に出る画像遷移ボタン)の寸法・表示条件・当たり判定(純粋関数、単体テスト対象)。**廃止しうる表示なので判定をここに閉じている**(上記「オーバーレイ矢印」) |
| `help.h` | 現在の `Keymap`(通常と選択中の 2 本)/ `Mousemap` から操作一覧の表示テキストを組み立てる(`buildHelpLines`)。固定テキストを持たないので README と drift しない。コマンドの表示名の正はこのファイルの表 1 つで、キーの節とマウスの節が共有する(単体テスト対象) |
| `SidebarState` | サイドバーの可視・モード (`SidebarMode`)・幅・スクロール量と、右端を掴む幅変更ドラッグの状態(`sidebar_state.h`。純粋な状態と幾何、単体テスト対象)。幅は設定 (ini) から来る素の `configuredWidth` と、操作一覧モードで `kHelpWidth` まで広げた表示上の `width` の 2 つを区別する。**一覧の中身も窓も知らない** ― 項目数・領域の高さのように外から決まる値は引数で受け取り、フルスクリーンで隠す判断とレイアウトの作り直しは `App` に残る |
| `PointerState` | マウス操作の進行状態(`pointer_state.h`。純粋な状態、単体テスト対象)。左右の役割 (`MouseButton` / `MouseRole` / `swap_buttons`)、最後のポインタ位置と窓内にいるか、パン中か、右クリックの押下位置とそこから決まる `MenuOnRelease`、ホイールの貯金。**画像も窓も知らない** ― 画素に触る編集ドラッグ (`EditDragState`) とオブジェクト操作 (`ObjectDragState`) は別に持つ |
| `EditDragState` | 編集ドラッグの進行状態(`edit_drag_state.h`。純粋な状態、単体テスト対象)。ドラッグ中か・始点と終点(画像座標)・押下位置から十分に動いたか・手書きの軌跡と Shift の直線アンカー(アンカーから先を 1 本に引き直す)。**画像もツールも窓も知らない** ― 画像座標への変換とクランプ・Shift での方向合わせ (`App::dragEndImage`)・どのツールを適用するかは `App` に残る |
| `ObjectDragState` | 注釈オブジェクトを掴んでいる間の状態(`object_drag_state.h`。純粋な状態、単体テスト対象)。進行中の操作 (`ObjectDragMode`)・掴んだ時点の注釈の写し・移動量・回転量・掴んだハンドル。変形は毎回この写しを基準に計算し直す(途中経過に差分を積むと誤差が溜まるため)。**注釈の一覧も画像も窓も知らない** ― 対象は `App::selected_` が指し、ハンドルのヒット判定と実際の変形 (`resizeAnnotation`) は `App` に残る |
| `EditStyle` | これから描く注釈の設定(`edit_style.h`。純粋な状態、単体テスト対象)。選択中のツール (`EditTool`) と、色・線幅・文字サイズ・フォント・塗り・枠線。値の丸め(線幅 1〜100px など)と ini の `[edit]` の読み取り (`applyConfig`) もここが持つ。**画像も注釈の一覧も窓も知らない** ― 次に作る 1 件へ写す (`applyTo`) だけで、何をどこへ描くか(種別・位置)と既にある注釈の書式変更は `App` に残る |
| `menu.h` | ポップアップメニューの組み立て(`MenuItem` の木と末端項目の一覧を作る純関数。単体テスト対象)。ツール切り替え (`buildEditMenu`)・リサイズのプリセット (`buildResizeMenu`)・サイドバー (`buildSidebarMenu`)・注釈オブジェクト (`buildObjectMenu`)・編集中テキストの書式 (`buildTextStyleMenu`) の 5 つと、フォント候補の解決 (`fontFamilyChoices`)。**メニューを出さないし、選ばれた結果も適用しない** ― 表示 (`IAppHost::showContextMenu`) と適用 (`App::applyEditChoice` など) は `App` に残る。実在するフォントの問い合わせも `FontAvailableFn` で受け取るので、描画側 (`IAnnotationRasterizer`) を知らない |
| `view_text.h` | 常に見えている表示文字列の組み立て(状態 → 文字列の純関数。単体テスト対象)。タイトルバー (`windowTitle`)・ステータスバー左側 (`statusText`)・カーソル位置の座標と色 (`pixelInfoText`) の 3 つ。**設定も描画もしない** ― `IAppHost::setTitle` を呼ぶのも `StatusBarView` に詰めるのも `App` に残る。窓の状態(フルスクリーンでバーを隠す判断)も、カーソルがビューポート内にいるかの判定も知らない |
| `EditHistory` | 取り消し・やり直しの履歴(`edit_history.h`。1段は `EditSnapshot` = 画像 + 注釈一覧、上限 `kLimit` = 10。純粋な状態、単体テスト対象)。ドラッグ中・テキスト編集中・キーでの連続移動中の「最初の1回だけ積む」判定の旗も持つ。画像そのものや表示状態は知らず、復元は `App::restoreFrom` が行う |
| `TextEditBuffer` | インプレース編集中の文字列・キャレット・選択範囲・部分書式(`text_edit.h`。UTF-8 バイト位置の純粋ロジック、単体テスト対象)。文字列を変えるたびに書式範囲を追従させる |
| `TextEditState` | インプレース編集の進行状態(`text_edit_state.h`。純粋な状態、単体テスト対象)。編集中か・対象の注釈 index・新規作成中か・`TextEditBuffer` 本体・キャレットの点滅相・ドラッグでの範囲選択・書式メニューの押下・IME の変換中文字列(とそれを混ぜた描画用のテキスト・書式・キャレット位置)。**注釈も画像も窓も知らない** ― 注釈への書き戻し・枠の実測・キャレット位置の通知はラスタライザの計測を要するため `App` に残る |
| `text_style.h` | 部分書式(色・太字・斜体・下線・フォント)の範囲リストとその編集の純関数。範囲の切り分け・正規化・編集への追従(`adjustTextStyles`)を担う(単体テスト対象) |
| `MainWindow` | Win32 メッセージ変換、フルスクリーン、ダイアログ(開く・保存・上書き確認・色選択)・編集メニュー(サブメニュー対応)・テキストのインプレース編集まわり(`WM_CHAR`・IME・キャレット点滅タイマー)(IAppHost 実装) |
| `RendererD2D` | BGRA ピクセル → ID2D1Bitmap(生成は `createD2DBitmap`。±1枚をGPU側にキャッシュ。枚数とバイト数の両方で上限。表示中の画像と `Kind::Image` 注釈の画素が同じキャッシュを通る)して描画。**GPU が扱える大きさの上限 (`GetMaximumBitmapSize`。機種により 8192 のこともある) を超える画像は `downscaleToFit` で縮小して載せ、転送元矩形はビットマップの実寸から取る**(縮むのは GPU 側のコピーだけで、保存・コピー・文字認識は元の大きさのまま)。注釈オブジェクト(`AnnotationsView`、選択枠・回転ハンドル含む)と選択領域のラバーバンド(`SelectionView`)もここで描く。図形の描画コードは焼き込みと共通(win/annotation_draw)。サイドバー・ステータスバーの文字は DirectWrite |
| `DecoderWic` | WIC で 32bpp PBGRA に統一デコード。EXIF 回転適用、16384px 超は縮小(**メモリのための上限**。16384<sup>2</sup> でも PBGRA で 1GB になる)。縮小したときは元の大きさを `DecodedImage::sourceWidth/sourceHeight` に残し、上書き保存の拒否とステータスバーの表示に使う。デコード失敗時は失敗した段階と HRESULT を文字列で返し、ステータスバーに出す。**カラーマネジメントは遅延式**: `decode` はプロファイルの有無だけ見て `colorPending` を立て、`decodeColorManaged`(`IWICColorTransform` で ICC → sRGB)が後から読み直す。色変換は 24MP で 0.5 秒ほどかかり、最初の表示を待たせたくないため(`[view] color_management`。既定 true)。多フレームは `probeSequence`(GIF → Animation、TIFF/ICO → Pages)・`decodePage`・`decodeAnimation`(GIF のメタデータを読んで core の `AnimationCompositor` へ渡す)で扱い、**ICO だけは表示順を大きい順に並べ替える**(index 0 = 最大 = `decode` が返すもの)|
| `PrinterWin` | GDI による印刷 (Ctrl+P)。`PrintDlg` で選ばせたプリンタの DC へ `StretchDIBits` で 1 ページ描く(プリンタ・用紙・向き・部数はダイアログ、余白と自動回転は `[print]`)。**印刷される大きさを超える画素は先に `downscaleToFit` で捨てる**(GDI の間引きより結果がきれいで、ドライバへ渡す量も減る)。ダイアログの取りやめと失敗を区別して返し、App がメッセージを出し分ける |
| `print_layout.h` | 用紙のどこに画像を置くかの計算(`layoutPrintImage`)。縦横比を保って印刷可能領域いっぱいに拡大縮小し、中央へ置く。**用紙より小さい画像も拡大する**(600dpi のプリンタで原寸に刷ると切手ほどにしかならないため)。90 度回した方が大きく刷れるなら回すかどうかも返す(`[print] auto_rotate`)。純粋関数で単体テスト対象 |
| `image_scale.h` | `downscaleToFit`(箱型フィルタで縦横を上限以下に縮小)。**描画側の上限に収めるために両レンダラが呼ぶ**。純粋関数で単体テスト対象。事前乗算なのでチャンネルをそのまま平均してよい。利用者が指示するリサイズは別の `resizeImage`(分離可能な三角フィルタ。**フィルタ半径を `max(1, 1/倍率)` にすると拡大=バイリニア・縮小=面積平均が 1 本のコードで書ける**)。`downscaleToFit` は巨大画像を開くたびに通る性能の効く経路なので統合せず箱型のまま残してある |
| `exif.h` | EXIF Orientation の読み取り(`readExifOrientation`、JPEG の APP1 と PNG の eXIf を自前パース。SDL 版のためだが core に置く)と適用(`applyExifOrientation`、32bpp バッファの回転・反転。表示回転の焼き込みにも流用する)。いずれも純粋関数で単体テスト対象。**`IWICBitmapFlipRotator` は使わない**: コーデックへ直結すると 90/270 度回転で出力行ごとにソースを引き直し、iPhone の 12〜24MP 写真で事実上停止するため。デコード完了後の連続バッファ上で回せば画素数に比例した時間で済む |
| `EncoderWic` | WIC で PNG/JPEG/BMP 保存 (Ctrl+S / Shift+Ctrl+S)。JPEG 品質は `EncodeOptions::jpegQuality` を ImageQuality へ渡す。`supports` は拡張子だけで可否を答える(上書き保存の可否をファイルへ手を付ける前に判断するため)。PNG は逆乗算してアルファ保持、JPEG/BMP は白背景に合成 |
| `ClipboardWin` | クリップボード読み書き。書き込みは CF_DIBV5(アルファ)+ CF_DIB(白合成24bpp)の2形式。読み取り (Ctrl+V) は CF_DIBV5 優先で、DIB → PBGRA 変換は core の `imageFromDib`(純粋関数、単体テスト対象) |
| `OcrEngineWinrt` | Windows.Media.Ocr による文字認識(`IOcrEngine` 実装)。**WinRT を C++/WinRT ではなく ABI で直接呼び、`combase.dll` は `LoadLibrary` で遅延解決する**。`windowsapp.lib` を静的リンクすると exe のインポートが増え、OCR を使わない起動でも DLL が読まれるため。認識器の生成も最初の実行まで遅延する。上限を超える画像は WIC で縮小し、座標は元のスケールへ戻す。文字が小さいときは 1 回目の行高から拡大率を決めて読み直す 2 パス構成(判断は core の `ocrRetryUpscale`)|
| `AnnotationD2D` | 図形(矩形・楕円・矢印・直線・手書き・連番マーカー)とテキスト(複数行可)と貼り付けた画像 (`Kind::Image`) を D2D/DirectWrite で WIC ビットマップへ AA 描画し、PBGRA overlay として返す(`IAnnotationRasterizer` 実装)。描画コードはライブ表示と共通の `win/annotation_draw` を使い、`AnnotationSpec::angleDeg` によるバウンディングボックス中心周りの回転にも対応。テキスト注釈の実測サイズ取得(`App::measureTextExtent`)にも使われる。トリミング・合成は core の `edit.cpp`(`cropImage` / `blendOverlay`)、注釈のヒットテスト・回転幾何は core の `annotation_edit.cpp`(いずれも純粋関数、単体テスト対象) |

## スレッドモデル

- **UI スレッド**: メッセージループ、描画、App の全状態。App はスレッド安全ではない
- **デコードワーカー (ImageCache 内の1本)**: `IImageDecoder::decode` と
  `decodeColorManaged`(色変換の読み直し)の実行のみ
  - UI → ワーカー: `ImageCache::requestNow / setPrefetch`(ミューテックス保護のキュー)。
    読み直しの予約はワーカー自身が積む(`colorPending` を見て `refine_` へ)
  - ワーカー → UI: `onDecoded` コールバック → `PostMessage(kMsgImageDecoded)` → `App::onDecodeCompleted`
  - 同じパスに対して**完了通知が 2 回来ることがある**(1 回目は未変換、2 回目は色変換後)。
    2 回目は `App::adoptRefinedImage` が画素だけ差し替える(ズーム・パン・注釈は保つ)。
    編集中は差し替えない
- **OCR ワーカー (OcrService 内の1本)**: `IOcrEngine::recognize` の実行のみ。同じ形をなぞる
  - UI → ワーカー: `OcrService::request`(ミューテックス保護の1件だけの枠)
  - ワーカー → UI: `onCompleted` コールバック → `PostMessage(kMsgOcrCompleted)` → `App::onOcrCompleted`
  - このスレッドは WinRT の MTA として初期化される。`OcrEngineWinrt` は非同期完了を
    ブロックして待つため、STA のスレッド(UI スレッド)から呼ぶとデッドロックする。
    そうなる前に失敗を返すようにしてあるが、呼ぶ側がこの経路を守ること
- **走査ワーカー (ScanService 内の1本)**: `IFileSystem::listImages` の実行のみ。同じ形をなぞる
  - UI → ワーカー: `ScanService::request`(ミューテックス保護の1件だけの枠)
  - ワーカー → UI: `onCompleted` コールバック → `PostMessage(kMsgScanCompleted)` → `App::onScanCompleted`
  - 走らせるのは**サブフォルダを再帰で辿るときだけ**。フォルダ直下の列挙は速いので
    UI スレッドで同期に済ませる(起動時は必ずこちらが先に走って表示を確定する)。
    このため `IFileSystem` の実装はスレッド安全であること
- 上記以外にワーカーは無く、境界はいずれも「キュー投入」と「完了を PostMessage」の2箇所だけ

## 起動シーケンス(高速化の要)

1. `wWinMain`: ウィンドウ生成(即表示、この時点で操作可能)
2. コマンドライン引数の画像を `ImageCache::requestNow` で即デコード開始(フォルダ列挙を待たない)
3. UI スレッドでフォルダ**直下**の列挙(自然順ソート)→ `ImageList` 確定
4. デコード完了通知で表示、隣接画像の先読み開始
5. 多フレームになりうる拡張子(`mayHaveMultipleFrames`)なら、表示に採用した時点で
   フレーム構成の調査を予約する。アニメーションと分かったら全フレームの展開まで裏で進み、
   終わったら再生が始まる(先読みでは調査しない)
6. `[view] recursive = true` のときだけ、`ScanService` がサブフォルダを裏で走査し、
   終わったら一覧を差し替える(表示中の画像はそのまま)

## 機能追加の手順(例: スライドショー)

1. `core/command.h` の `Command` に `ToggleSlideshow` を追加
2. `core/app.cpp` の `App::execute` にハンドラを追加(タイマーは IAppHost に API を足す)
3. `core/keymap.cpp` の `kCommandNames`(ini 名)とデフォルトキー表に追加
   (オブジェクト選択中だけ効く操作なら `keyScopeOf` を `KeyScope::Selection` にし、
   既定は `Keymap::defaults` ではなく `selectionDefaults` へ書く)
4. `core/help.cpp` の `kCommandLabels` に表示名を追加し、操作一覧の節へ `row()` を足す
   (表示名が無いと `F1` の一覧に出ない。マウスへの割り当ても同じ表を通る)
5. マウスにも既定で割り当てるなら `core/mousemap.cpp` の `Mousemap::defaults`
   (ini 名は `kCommandNames` と共通なので、書けるようにするだけなら何もしなくてよい)
6. 必要なら `tests/core_tests.cpp` にテストを追加

## v0.2 以降の候補

- ごみ箱削除 / EXIF 情報表示
- stb_image 系フォールバックデコーダ(`IImageDecoder` の別実装)※ SDL 版では実装済み (decoder_stb)
- SDL 版の編集対応(コンテキストメニュー・色選択の自前 UI + CPU ラスタライザとテキスト計測)
