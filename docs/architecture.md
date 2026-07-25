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
| `AnnotationStub` | `AnnotationD2D` | **未実装**(ラスタライズ・テキスト計測とも)。ツールメニュー(コンテキストメニュー・色選択)やテキストのインプレース編集も未対応のため、SDL 版は閲覧専用 |

SDL 版の既知の制限: 注釈編集ができない(`WindowSdl` は編集役のボタン
(`App::mouseRole` が `MouseRole::Edit` を返すほう)のイベントを App へ渡さない。
ツールメニューも注釈の描画も無いため、繋ぐと見えない注釈だけが増える)、
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

マウス操作も同じ形で App に集まる。ウィンドウ層は**物理ボタン**(`MouseButton`)を
そのまま `App::onMouseDown` / `onMouseUp` / `onMouseMove` へ渡すだけで、どちらのボタンが
何をするかは App が `mouseRole` で振り分ける(下記「マウスボタンの役割」)。
編集役のボタンのドラッグは **現在のツール**(`EditTool`、`App::tool_`)を選択領域へ
適用する。トリミングは core の `cropImage` で `current_` を差し替え、図形・テキストは
**非破壊の注釈オブジェクト**(`std::vector<AnnotationSpec>`)として App が保持する。
注釈は描画時に `AnnotationsView` として RendererD2D へ渡されベクター描画で重なり
(`current_` のピクセルは変更しない)、保存 (Ctrl+S)・コピー (Ctrl+C) 時にだけ
`App::compositeImage` が `IAnnotationRasterizer` + `blendOverlay` で合成する。
トリミングは注釈座標を平行移動してオブジェクトのまま維持する。

**ツールは事前に選ぶ**(動詞 → 目的語)。同じ範囲に対して何をするかを後から選ぶ形では、
矢印を5本引くような反復のたびにメニューを往復することになり、ドラッグ中に実物を
見せられないため。切り替えは注釈のない場所での右クリック(ドラッグなし)で開く
ツールメニュー(入れ替え設定にかかわらず常に右クリック)、または
`Command::SelectTool*`(既定のキーは持たず blinker.ini で割り当て)。
起動時のツールは blinker.ini の `[edit] tool`。
ツールメニューは階層構造(`MenuItem` の木、選択結果は末端項目の深さ優先通し番号)で、
設定系の項目(線の太さ・文字サイズ・フォント・色 = `IAppHost::showColorPicker`)を
選んだ場合はメニューを再表示し、設定を整えてからツールを選べる(設定は新規作成の既定値)。
フォントの候補は `IAnnotationRasterizer::hasFontFamily` で実在するものだけへ絞る
(起動時には呼ばず、メニューを開いたときだけ問い合わせる)。
トリミングだけは一度きりの操作なので、実行したら直前の図形ツールへ戻す。

ドラッグ中は `App::makeAnnotationSpec` が確定後と同じ `AnnotationSpec` を組み立て、
`AnnotationsView::preview` として実物をプレビュー描画する。形の定まらない
トリミング・テキストだけは従来どおりラバーバンド (`SelectionView`) を出す。

手書き(ペン・マーカー = `Kind::Pen`)だけは選択領域ではなく**軌跡**が図形になる。
編集ドラッグ中の `onMouseMove` が通過点を `App::penPoints_` へ溜め(`appendPenPoint` が画面 2px
未満の動きを間引く)、`AnnotationSpec::points`(回転前の画像座標)として持つ。
`p1`/`p2` は点列の bbox に同期させ(`updatePenBounds`)、選択枠・ヒットテスト・回転中心・
ラスタライズ領域は他の種別と同じ経路を通す。移動・リサイズでは bbox と同じだけ点列も
動かす/拡縮する(`translateAnnotation` / `resizeAnnotation`)。ヒットテストは
**線の近傍だけ**で、bbox 内部は当たりにしない(囲むように描いた線の内側が丸ごと
掴めると、下の図形を選べなくなるため)。マーカーはペンと同じ種別で、線幅 4 倍・
不透明度 40%(`strokeAlpha`)という既定値の違いしかない。
連番マーカー (`Kind::Number`) は数字入りの円で、番号は `App::nextMarkerNumber` が
既存の注釈から数え直す(状態を持たないので undo・削除のあとも番号が詰まる)。
円を保つためドラッグは常に正方形へ寄せ、ハンドルも四隅だけにしてある。
現在のツールはステータスバー左側に表示する(モードが見えないと誤操作になるため)。

Shift ドラッグの寄せ方は種別で変える(`App::dragEndImage`)。矩形・楕円などは
選択領域を正方形にする (`constrainToSquare`) が、直線・矢印は bbox ではなく**線の向き**を
揃えたいので、始点から見て一番近い水平・垂直・45 度へ寄せる (`constrainToAxis`)。
どちらも移動量の小さいほうに合わせるので、画像の端まで引いても結果は画像内に収まる。
`onShiftChanged` がドラッグ中の Shift の押し引きを拾い、マウスを止めたままでも
プレビューが追従する。

追加済みの注釈はパン役のボタンのクリックで選択して編集できる(ヒットテスト・回転・
リサイズの幾何は core の `annotation_edit.cpp`、純粋関数、単体テスト対象):
ドラッグで移動、四隅・辺のハンドルでサイズ変更(Shift で縦横比維持。回転中は
反対側のアンカーを固定。Line/Arrow は端点ドラッグで、Shift 中は固定端から見て
水平・垂直・45 度へ寄せる。Text は幅のみで高さは
ドラッグ確定時に実測へ正規化)、選択枠上の回転ハンドルで自由回転(Shift で 15° スナップ)、
右クリックでオブジェクトメニュー(回転角度プリセット・太さ・文字サイズ・フォント・
色・削除、手書きは線の不透明度、連番マーカーは番号)。
Text 注釈を選択中の `Ctrl+B` だけは `App::onKey` が Keymap より先に横取りし、
テキスト全体の太字を切り替える(`toggleSelectedTextBold`)。既定では
`Command::ToggleSidebar` と同じキーだが、選んでいるオブジェクトへの操作を優先する。
インプレース編集中の Ctrl+B/I/U が Keymap を通らないのと同じ扱いで、
選択 → 編集を行き来しても Ctrl+B の意味が変わらない。
Ctrl+Z で1段階ずつ取り消し、Ctrl+Y(Shift+Ctrl+Z も可)でやり直せる(履歴は画像 +
注釈一覧のスナップショット、undo・redo とも上限10)。undo は現在の状態を redo 側へ、
redo は undo 側へ積み替えるだけの対称な操作 (`App::restoreFrom`) で、
新しい編集を積む (`pushUndoState`) と redo 履歴は捨てる(分岐した未来は残さない)。
保存は Ctrl+S のみで元ファイルは自動では書き換えない。

### マウスボタンの役割

物理ボタン (`MouseButton`) と役割 (`MouseRole`) を分けてあり、対応は
blinker.ini の `[mouse] swap_buttons` で入れ替えられる(`App::mouseRole`)。

| 役割 | 既定 | 入れ替え時 | 内容 |
|---|---|---|---|
| `Pan` | 左 | 右 | 何も掴まなかったドラッグで画像をパンする |
| `Edit` | 右 | 左 | 何も掴まなかったドラッグで現在のツールを実行する |
| オブジェクト | 左 | 左 | 注釈の選択・移動・回転・サイズ変更、キャレット移動(**入れ替えない**) |
| メニュー | 右 | 右 | ツール切り替え・オブジェクト・書式メニュー(**入れ替えない**) |

入れ替えの対象が「パン」と「編集ドラッグ」だけなのは、メニューを開くボタンが
状況で変わると押せなくなるため。右ボタンは常にメニュー役も兼ね、ドラッグ量が
`kDragThresholdPx` 未満のまま離されたときだけ `showPointerMenu` が開く
(既定ではその右ボタンが編集役でもあるので、編集ドラッグを始めていても
閾値未満なら何も作らずメニューになる)。サイドバーの項目クリックは画像への操作では
ないので、これも入れ替えず常に左ボタン。

サイドバーの幅は右端(境界をまたぐ `kSidebarResizeGripPx` の帯)を左ボタンで掴んで
変えられる。`onMouseDown` はこの判定を項目クリックより先に見る(境界際のクリックで
画像が切り替わってしまわないように)。掴んだ位置からの総移動量で幅を決めるので、
下限・上限に当たってポインタが端から離れても戻せば追従する。下限はモードごとに
`sidebarOffset()` が返す幅と揃える(操作一覧は `kHelpSidebarWidth`)。上限は
`kMaxSidebarWidth` と「窓幅 - `kMinViewportWidth`」の狭いほう。ini の
`sidebar_width` は起動時の幅で、ドラッグでの変更は保存しない。

オブジェクトを掴む操作も入れ替えない。既存の図形を選ぶのに右クリックが要るのは
他のペイント系ソフトと食い違って戸惑うため、左ボタンのままにしてある。そのため
`onMouseDown` は**左ボタンだけ**まず `beginObjectGrab` を通し、掴めなかったときに
初めて `mouseRole` どおりの動き(パン / 編集ドラッグ)へ落とす。役割は「掴めなかった
ときに何をするか」を決めるだけ、という位置づけ。解放も対称で、`endObjectGrab` は
左ボタンのときだけ呼ぶ(入れ替え時の左は `endEditDrag` も通るが、掴んでいたなら
`selecting_` が false なので素通りする)。

この結果、入れ替え時は既存の図形の上から新しい図形を描き始められない(左が選択に
なるため)。図形の無いところから描き始めればよいので、選択できないより実害が小さい
という判断。

振り分けは core に閉じている。ウィンドウ層はボタンの押下・解放・移動をそのまま
App へ渡すだけで、パンの差分計算(`lastPointerScreen_`)もドラッグ中かの判定も
App が持つ(役割が設定で変わるので、ウィンドウ層に状態を置くと二重管理になる)。
SDL 版だけは編集役のボタンを App へ渡さない(注釈編集が未対応のため)。

### テキストのインプレース編集

Text 注釈は PowerPoint のテキストボックスと同じく**画像上で直接編集する**
(モーダルなテキスト入力ダイアログは持たない)。テキストツールで編集ドラッグすると
ドラッグした矩形が空のテキストボックスになって編集が始まり、既存のテキストは
ダブルクリック(またはオブジェクトメニューの「テキストを編集」)で編集に入る。

- 文字列・キャレット・選択範囲は core の `TextEditBuffer`(`text_edit.h`、UTF-8 の
  バイト位置で持つ純粋ロジック、単体テスト対象)。
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
- 変換中文字列は **`TextEditBuffer` には入れず**、`App::textEditDisplayText` が
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
  **入力した文字は直前の文字の書式を継ぐ**(一般的なエディタと同じ)。
- 操作は編集中の Ctrl+B(太字)・Ctrl+I(斜体)・Ctrl+U(下線)と、選択範囲の上での
  右クリックで出る書式メニュー(太字・斜体・下線・フォント・文字色…)。右クリックは
  通常なら編集を確定するが、選択範囲の上でだけは確定せずメニューを出す。
  フォントの候補と既定の解決はツールメニューと同じ(`App::fontFamilyChoices`)。
- 描画は Windows のみ。太字・斜体・下線・フォントは文字送りと行高に影響するため
  `createAnnotationTextLayout` がレイアウト生成時に適用し(キャレット位置・
  ヒットテストの計測も同じレイアウトを通るので表示と一致する)、色は見た目だけなので
  描画時に `applyTextColorEffects` が `IDWriteTextLayout::SetDrawingEffect` へ
  ブラシを載せる(D2D の既定テキストレンダラがその範囲だけ別ブラシで描く)。
  ブラシはデバイス依存なので、描画に使うレンダーターゲットで作ること。

## 主要コンポーネント

| コンポーネント | 責務 |
|---|---|
| `App` | 状態機械の中心。Command を受けて状態更新、host へ再描画依頼。ステータスバー (`StatusBarView`) とサイドバー (`SidebarView`、可視範囲の項目のみ) の表示内容もここで組み立てる。サイドバーは `SidebarMode` でファイル名一覧と操作一覧 (F1) を切り替える(レンダラからは同じ文字列リストに見える)。貼り付け画像はフォルダ一覧から独立した表示状態(`clipboardImage_`)で持ち、移動系コマンドで一覧表示へ戻る。編集(現在のツール `EditTool` と編集ドラッグでの適用、プレビュー・`SelectionView`・注釈オブジェクトの選択/移動/回転ドラッグ状態・undo 履歴)もここで管理し、画像切替で破棄する |
| `Viewport` | ズーム/パン/フィット/回転の座標変換(純粋計算、テスト容易) |
| `ImageList` | フォルダ内画像の一覧・現在位置・先読み候補の順序付け |
| `ImageCache` | ワーカースレッド1本で非同期デコード。LRU(既定: 8枚 or 512MB) |
| `Keymap` | KeyChord → Command。デフォルト表 + ini 上書き。逆引き (`chordsFor` / `chordToString`) も持ち、操作一覧の生成に使う |
| `help.h` | 現在の `Keymap` から操作一覧の表示テキストを組み立てる(`buildHelpLines`)。固定テキストを持たないので README と drift しない(単体テスト対象) |
| `TextEditBuffer` | インプレース編集中の文字列・キャレット・選択範囲・部分書式(`text_edit.h`。UTF-8 バイト位置の純粋ロジック、単体テスト対象)。文字列を変えるたびに書式範囲を追従させる |
| `text_style.h` | 部分書式(色・太字・斜体・下線・フォント)の範囲リストとその編集の純関数。範囲の切り分け・正規化・編集への追従(`adjustTextStyles`)を担う(単体テスト対象) |
| `MainWindow` | Win32 メッセージ変換、フルスクリーン、ダイアログ(開く・保存・色選択)・編集メニュー(サブメニュー対応)・テキストのインプレース編集まわり(`WM_CHAR`・IME・キャレット点滅タイマー)(IAppHost 実装) |
| `RendererD2D` | BGRA ピクセル → ID2D1Bitmap(±1枚をGPU側にキャッシュ)して描画。注釈オブジェクト(`AnnotationsView`、選択枠・回転ハンドル含む)と選択領域のラバーバンド(`SelectionView`)もここで描く。図形の描画コードは焼き込みと共通(win/annotation_draw)。サイドバー・ステータスバーの文字は DirectWrite |
| `DecoderWic` | WIC で 32bpp PBGRA に統一デコード。EXIF 回転適用、16384px 超は縮小。デコード失敗時は失敗した段階と HRESULT を文字列で返し、ステータスバーに出す |
| `exif.h` | EXIF Orientation の適用(`applyExifOrientation`、32bpp バッファの回転・反転。純粋関数で単体テスト対象)。**`IWICBitmapFlipRotator` は使わない**: コーデックへ直結すると 90/270 度回転で出力行ごとにソースを引き直し、iPhone の 12〜24MP 写真で事実上停止するため。デコード完了後の連続バッファ上で回せば画素数に比例した時間で済む |
| `EncoderWic` | WIC で PNG/JPEG/BMP 保存 (Ctrl+S)。PNG は逆乗算してアルファ保持、JPEG/BMP は白背景に合成 |
| `ClipboardWin` | クリップボード読み書き。書き込みは CF_DIBV5(アルファ)+ CF_DIB(白合成24bpp)の2形式。読み取り (Ctrl+V) は CF_DIBV5 優先で、DIB → PBGRA 変換は core の `imageFromDib`(純粋関数、単体テスト対象) |
| `AnnotationD2D` | 図形(矩形・楕円・矢印・直線・手書き・連番マーカー)とテキスト(複数行可)を D2D/DirectWrite で WIC ビットマップへ AA 描画し、PBGRA overlay として返す(`IAnnotationRasterizer` 実装)。描画コードはライブ表示と共通の `win/annotation_draw` を使い、`AnnotationSpec::angleDeg` によるバウンディングボックス中心周りの回転にも対応。テキスト注釈の実測サイズ取得(`App::measureTextExtent`)にも使われる。トリミング・合成は core の `edit.cpp`(`cropImage` / `blendOverlay`)、注釈のヒットテスト・回転幾何は core の `annotation_edit.cpp`(いずれも純粋関数、単体テスト対象) |

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
- SDL 版の編集対応(コンテキストメニュー・色選択の自前 UI + CPU ラスタライザとテキスト計測)
- SDL 版の EXIF 回転対応(JPEG の Orientation タグを自前パース)
