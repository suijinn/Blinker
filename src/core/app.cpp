#include "core/app.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <utility>

#include "core/annotation_edit.h"
#include "core/edit.h"
#include "core/exif.h"
#include "core/help.h"
#include "core/image_scale.h"
#include "core/ocr_text.h"
#include "core/pixel_convert.h"
#include "core/str_util.h"
#include "core/unicode.h"
#include "core/version.h"
#include "core/view_text.h"

namespace blinker {

namespace fs = std::filesystem;

namespace {

constexpr unsigned kMessageDurationMs = 3000;

// 2 つのパスが同じファイルを指すかを見る(Windows に合わせて大文字小文字を無視する)。
// 畳むのは ASCII だけだが、日本語のファイル名は大小の区別を持たないので実用上足りる
bool samePath(const fs::path& a, const fs::path& b) {
    return toLower(pathToUtf8(a)) == toLower(pathToUtf8(b));
}

// 上書き保存を断る理由を返す(断らないなら空文字列)。
// 巨大画像は表示できる大きさへ縮小して取り込まれるので、それで元ファイルを潰すと
// 失った画素は取り戻せない。名前を付けて保存なら許す
std::string overwriteBlockedReason(const DecodedImage& image) {
    if (!image.downscaled()) return {};
    return std::format("元が大きい({} x {})ため表示用に {} x {} へ縮小してあります。"
                       "上書きすると画素が失われるので、名前を付けて保存を使ってください",
                       image.sourceWidth, image.sourceHeight, image.width, image.height);
}

// Viewport の表示回転を画素へ焼き込む(保存・コピー・文字認識用)。回転は表示状態で
// current_ のピクセルには入っていないため、外へ出すときにここで反映する。
// EXIF Orientation の 6 / 3 / 8 が時計回り 90 / 180 / 270 度に一致するので流用する
bool bakeRotation(DecodedImage& image, int rotationDegrees) {
    switch (((rotationDegrees / 90) % 4 + 4) % 4) {
    case 1: return applyExifOrientation(image, 6);
    case 2: return applyExifOrientation(image, 3);
    case 3: return applyExifOrientation(image, 8);
    default: return false;
    }
}

// 図形ツールが作る注釈の種別。Ocr は注釈を作らないので Rect を返す(呼ばれない)。
// ペンとマーカーは同じ Pen 注釈で、違いは線幅と不透明度だけ(makeAnnotationSpec が付ける)
AnnotationSpec::Kind kindOfTool(EditTool tool) {
    switch (tool) {
    case EditTool::Ellipse: return AnnotationSpec::Kind::Ellipse;
    case EditTool::Arrow:   return AnnotationSpec::Kind::Arrow;
    case EditTool::Line:    return AnnotationSpec::Kind::Line;
    case EditTool::Pen:
    case EditTool::Marker:  return AnnotationSpec::Kind::Pen;
    case EditTool::Number:  return AnnotationSpec::Kind::Number;
    case EditTool::Text:    return AnnotationSpec::Kind::Text;
    case EditTool::Ocr:
    case EditTool::Rect:    break;
    }
    return AnnotationSpec::Kind::Rect;
}

} // namespace

App::App(IAppHost& host, IFileSystem& fileSystem, ImageCache& cache, IClipboard& clipboard,
         IImageEncoder& encoder, IAnnotationRasterizer& rasterizer, OcrService& ocr,
         IPrinter& printer, ScanService& scan)
    : host_(host),
      fileSystem_(fileSystem),
      cache_(cache),
      clipboard_(clipboard),
      encoder_(encoder),
      rasterizer_(rasterizer),
      ocr_(ocr),
      printer_(printer),
      scan_(scan) {}

void App::applyConfig(const Config& config) {
    // [keys] は 1 セクションのまま、文脈ごとの表それぞれに渡す(自分に属さない
    // コマンドの記述は読み飛ばされるので、利用者は表の区別を意識しなくてよい)
    keymap_.applyConfig(config.section("keys"), KeyScope::Global);
    selectionKeymap_.applyConfig(config.section("keys"), KeyScope::Selection);
    // [mouse] はコマンド割り当てと swap_buttons が同居する(後者はコマンド名として
    // 解決されないので Mousemap 側では無視される)
    mousemap_.applyConfig(config.section("mouse"));
    backgroundRGB_ = config.getColorRGB("view", "background", backgroundRGB_);
    viewport_.setFitUpscale(config.getBool("view", "fit_upscale", false));
    prefetchRadius_ = std::clamp(config.getInt("view", "prefetch_radius", prefetchRadius_), 0, 8);
    statusBarEnabled_ = config.getBool("view", "statusbar", statusBarEnabled_);
    sidebar_.setEnabled(config.getBool("view", "sidebar", sidebar_.enabled()));
    sidebar_.setConfiguredWidth(static_cast<float>(config.getInt(
        "view", "sidebar_width", static_cast<int>(sidebar_.configuredWidth()))));
    helpHintEnabled_ = config.getBool("view", "help_hint", helpHintEnabled_);
    // アニメーションの再生設定(展開の上限は ImageCache 側で読む)
    animationOptions_ = animationOptionsFromConfig(config);
    // 一覧の並び順と再帰。applyConfig は openPath より前に呼ばれるので最初の列挙から効く
    if (const auto key = sortKeyFromIniName(config.get("view", "sort"))) sortOrder_.key = *key;
    sortOrder_.descending = config.getBool("view", "sort_descending", sortOrder_.descending);
    recursive_ = config.getBool("view", "recursive", recursive_);
    encodeOptions_.jpegQuality =
        std::clamp(config.getInt("save", "jpeg_quality", encodeOptions_.jpegQuality), 1, 100);
    // 上書き保存(Ctrl+S)は元の画像を失うので既定では確認する
    confirmOverwrite_ = config.getBool("save", "confirm_overwrite", confirmOverwrite_);
    // 未保存の編集がある間の画像切り替えを断る(false で従来どおり黙って破棄する)
    lockNavigation_ = config.getBool("edit", "lock_navigation", lockNavigation_);
    // 印刷の余白は用紙の印刷可能領域からさらに空ける分(0-50mm)
    printOptions_.marginMm = static_cast<float>(std::clamp(
        config.getInt("print", "margin_mm", static_cast<int>(printOptions_.marginMm)), 0, 50));
    printOptions_.autoRotate = config.getBool("print", "auto_rotate", printOptions_.autoRotate);
    // 端に近づくと出る画像遷移用の矢印(使用感が合わなければ false で消せる)
    navArrowsEnabled_ = config.getBool("view", "nav_arrows", navArrowsEnabled_);
    // パンと編集の左右を入れ替える(メニューは入れ替えず常に右クリック)
    pointer_.setSwapButtons(config.getBool("mouse", "swap_buttons", pointer_.swapButtons()));
    // 水平ホイールを 1 段と数えるまでのノッチ数(トラックボールの誤爆対策)
    pointer_.setHorizontalThreshold(static_cast<float>(std::clamp(
        config.getInt("mouse", "wheel_horizontal_threshold",
                      static_cast<int>(pointer_.horizontalThreshold())),
        1, 10)));
    style_.applyConfig(config);
    applyLayout();
}

bool App::showHelpHint() {
    if (!helpHintEnabled_) return false;
    // 既に一覧が出ているなら案内は不要
    if (sidebar_.showing(SidebarMode::Help)) return false;
    const std::string keys = keysLabel(keymap_, Command::ToggleHelp);
    if (keys.empty()) return false;  // ini で外されているなら案内しない
    const std::string hint = std::format("{} で操作一覧", keys);
    // キーリピートで押しっぱなしのときに毎フレーム再描画を要求しない
    if (message_ == hint) return false;
    showMessage(hint);
    return true;
}

void App::showStartupHint() {
    showHelpHint();
}

void App::openPath(const fs::path& path) {
    // ドラッグ&ドロップ・Ctrl+O・「プログラムから開く」も表示中の画像を捨てる。
    // 一度きりの明示的な操作なので、黙って断らずダイアログで訊く
    // (無反応にすると故障に見えるため。矢印での移動は guardEditLock の側)
    if (!confirmEditLock(std::format("{} を開きます。", pathToUtf8(path.filename())))) return;
    std::error_code ec;
    const bool isDirectory = fs::is_directory(path, ec);
    const fs::path dir = isDirectory ? path : path.parent_path();
    // フォルダ列挙を待たずに表示対象のデコードを開始する(起動を最速にするため)
    if (!isDirectory) cache_.requestNow(path);
    listRoot_ = dir;
    // ここでは再帰が有効でも直下だけを同期列挙する。ツリー全体を UI スレッドで歩くと
    // 最初の 1 枚が出るまで固まるため、全体は走査ワーカーへ回して後から差し替える
    ListResult listed = fileSystem_.listImages(dir, {false, kMaxListFiles});
    entries_ = std::move(listed.entries);
    listTruncated_ = listed.truncated;
    current_.reset();
    origin_.clear();
    applyListOrder(isDirectory ? fs::path{} : path);
    refreshCurrent();
    requestScan(false);  // 起動時の走査は黙って差し替える(件数の通知は出さない)
}

void App::applyListOrder(const fs::path& keep) {
    order_ = sortedOrder(entries_, sortOrder_);
    std::vector<fs::path> paths;
    paths.reserve(order_.size());
    for (const size_t i : order_) paths.push_back(entries_[i].path);
    list_.set(std::move(paths), keep);
}

void App::relist() {
    // 位置の正は「一覧の現在位置」で、表示中の画像 (origin_.path()) ではない。
    // デコード待ちの間は両者がずれており、表示中の画像を基準にすると
    // 進行中の画像送りが取り消されてしまう
    const fs::path keep = list_.empty() ? fs::path{} : list_.current();
    // 貼り付け画像の表示中は一覧の位置に意味が無いので先頭へ寄せる(表示はそのまま)
    applyListOrder(origin_.fromClipboard() ? fs::path{} : keep);
    // 求めた位置に着けなかった(一覧が空だった・現在の画像が消えた)ときだけ表示を
    // 切り替える。着けたなら refreshCurrent は呼ばない — 編集中の内容を捨ててしまうため
    if (!origin_.fromClipboard() && !list_.empty() && !samePath(list_.current(), keep)) {
        refreshCurrent();
        return;
    }
    scrollSidebarToCurrent();
    updatePrefetch();
    updateTitle();
    host_.requestRedraw();
}

void App::requestScan(const bool announce) {
    if (!recursive_ || listRoot_.empty()) {
        scanGeneration_ = 0;  // 走査中の結果が届いても捨てる
        return;
    }
    scanAnnounce_ = announce;
    scanGeneration_ = scan_.request(listRoot_, {true, kMaxListFiles});
}

void App::onScanCompleted() {
    const auto done = scan_.takeResult();
    // 予約し直した後に届いた古い結果(再帰を切った・別のフォルダを開いた)は捨てる
    if (!done || done->generation != scanGeneration_) return;
    scanGeneration_ = 0;
    entries_ = std::move(done->result.entries);
    listTruncated_ = done->result.truncated;
    relist();
    if (listTruncated_) {
        showMessage(std::format("{} 件で打ち切りました(サブフォルダの画像が多すぎます)",
                                kMaxListFiles));
    } else if (scanAnnounce_) {
        showMessage(std::format("サブフォルダを含める ({} 件)", list_.size()));
    }
}

void App::setSortOrder(const SortOrder order) {
    sortOrder_ = order;
    relist();
    showMessage(std::format("並び順: {}", sortOrderLabel(sortOrder_)));
}

void App::selectSortKey(const SortKey key) {
    // 同じキーを選び直したら向きを反転する(エクスプローラの列ヘッダと同じ)。
    // 別のキーへ移るときは、日時だけ「新しい順」を既定にする(写真では大抵こちら)
    if (key == sortOrder_.key) {
        setSortOrder({key, !sortOrder_.descending});
        return;
    }
    setSortOrder({key, key == SortKey::Date});
}

void App::setRecursive(const bool enabled) {
    if (recursive_ == enabled) return;
    recursive_ = enabled;
    host_.requestRedraw();  // ステータスバーの表示が変わる
    if (listRoot_.empty()) return;
    if (enabled) {
        requestScan(true);
        showMessage("サブフォルダを検索しています...");
        return;
    }
    // 直下だけの列挙は同期で十分速い(走査ワーカーを起こす必要がない)。
    // 走査中だった結果は届いても捨てる
    scanGeneration_ = 0;
    ListResult listed = fileSystem_.listImages(listRoot_, {false, kMaxListFiles});
    entries_ = std::move(listed.entries);
    listTruncated_ = listed.truncated;
    relist();
    showMessage(std::format("サブフォルダを含めない ({} 件)", list_.size()));
}

void App::execute(Command command) {
    // 矢印での移動が途切れたら次からは新しい undo 段にする(連なりの区切り)。
    // ここに置くと、間に挟まる操作(取り消し・選択解除・ツール切り替え…)を
    // すべて区切りとして拾える
    if (keyScopeOf(command) != KeyScope::Selection) history_.resetKeyMove();
    switch (command) {
    // 遷移ロックの判定は list_ を動かす前に済ませる(next/prev は位置を書き換えるため)
    case Command::NextImage:
        if (guardEditLock("画像を切り替えられません")) navigate(list_.next());
        break;
    case Command::PrevImage:
        if (guardEditLock("画像を切り替えられません")) navigate(list_.prev());
        break;
    case Command::FirstImage:
        if (guardEditLock("画像を切り替えられません")) navigate(list_.first());
        break;
    case Command::LastImage:
        if (guardEditLock("画像を切り替えられません")) navigate(list_.last());
        break;
    case Command::TogglePlay:
        executeTogglePlay();
        break;
    case Command::NextFrame:
        stepFrame(1);
        break;
    case Command::PrevFrame:
        stepFrame(-1);
        break;
    case Command::ZoomIn:
        viewport_.zoomStep(true);
        onViewChanged();
        break;
    case Command::ZoomOut:
        viewport_.zoomStep(false);
        onViewChanged();
        break;
    case Command::ZoomFit:
        viewport_.fit();
        onViewChanged();
        break;
    case Command::ZoomActual:
        viewport_.actualSize();
        onViewChanged();
        break;
    case Command::PanLeft:
        panBy(kPanStepPx, 0);
        break;
    case Command::PanRight:
        panBy(-kPanStepPx, 0);
        break;
    case Command::PanUp:
        panBy(0, kPanStepPx);
        break;
    case Command::PanDown:
        panBy(0, -kPanStepPx);
        break;
    case Command::RotateCW:
        viewport_.rotate(1);
        onViewChanged();
        break;
    case Command::RotateCCW:
        viewport_.rotate(-1);
        onViewChanged();
        break;
    case Command::ToggleFullscreen:
        host_.setFullscreen(!host_.isFullscreen());
        break;
    case Command::OpenFile:
        if (const auto path = host_.showOpenDialog()) openPath(*path);
        break;
    case Command::CopyImage:
        executeCopyImage();
        break;
    case Command::CopyPath:
        if (origin_.fromClipboard() || list_.empty()) {
            showMessage("コピーするパスがありません");
        } else if (clipboard_.setText(pathToUtf8(list_.current()))) {
            showMessage("パスをコピーしました: " + pathToUtf8(list_.current()));
        } else {
            showMessage("パスのコピーに失敗しました");
        }
        break;
    case Command::CopyFile:
        // コピーされるのはディスク上の元ファイル(未保存の編集内容は含まれない)
        if (origin_.fromClipboard() || list_.empty()) {
            showMessage("コピーするファイルがありません");
        } else if (clipboard_.setFiles({list_.current()})) {
            showMessage("ファイルをコピーしました: " + pathToUtf8(list_.current()));
        } else {
            showMessage("ファイルのコピーに失敗しました");
        }
        break;
    case Command::CopyOcrText:
        // 注釈は焼き込まない。読みたいのは元画像の文字で、上に描いた図形ではない
        requestOcr(current_);
        break;
    case Command::PasteImage:
        executePasteImage();
        break;
    case Command::PasteObject:
        executePasteObject();
        break;
    case Command::SaveImage:
        executeSaveOverwrite();
        break;
    case Command::SaveImageAs:
        executeSaveAs();
        break;
    case Command::PrintImage:
        executePrint();
        break;
    case Command::ResizeImage:
        // 大きさはメニューのプリセットから選ぶ(数値入力ダイアログは持たない)
        showResizeMenu(pointer_.lastScreen());
        break;
    case Command::SortByName:
        selectSortKey(SortKey::Name);
        break;
    case Command::SortByDate:
        selectSortKey(SortKey::Date);
        break;
    case Command::SortBySize:
        selectSortKey(SortKey::Size);
        break;
    case Command::SortByExtension:
        selectSortKey(SortKey::Extension);
        break;
    case Command::ToggleSortDescending:
        setSortOrder({sortOrder_.key, !sortOrder_.descending});
        break;
    case Command::CycleSortKey:
        selectSortKey(nextSortKey(sortOrder_.key));
        break;
    case Command::ToggleRecursive:
        setRecursive(!recursive_);
        break;
    case Command::Undo:
        executeUndo();
        break;
    case Command::Redo:
        executeRedo();
        break;
    case Command::DiscardEdits:
        executeDiscardEdits();
        break;
    case Command::DeleteAnnotation:
        deleteSelectedAnnotation();
        break;
    case Command::MoveObjectLeft:
        moveSelectedObject({-1, 0});
        break;
    case Command::MoveObjectRight:
        moveSelectedObject({1, 0});
        break;
    case Command::MoveObjectUp:
        moveSelectedObject({0, -1});
        break;
    case Command::MoveObjectDown:
        moveSelectedObject({0, 1});
        break;
    case Command::CropToSelection:
        cropToSelection();
        break;
    case Command::SelectToolRect:
        setTool(EditTool::Rect);
        break;
    case Command::SelectToolEllipse:
        setTool(EditTool::Ellipse);
        break;
    case Command::SelectToolArrow:
        setTool(EditTool::Arrow);
        break;
    case Command::SelectToolLine:
        setTool(EditTool::Line);
        break;
    case Command::SelectToolPen:
        setTool(EditTool::Pen);
        break;
    case Command::SelectToolMarker:
        setTool(EditTool::Marker);
        break;
    case Command::SelectToolNumber:
        setTool(EditTool::Number);
        break;
    case Command::SelectToolText:
        setTool(EditTool::Text);
        break;
    case Command::SelectToolOcr:
        setTool(EditTool::Ocr);
        break;
    case Command::ToggleSidebar: {
        // 操作一覧が出ている間は、閉じるのではなくファイル名一覧へ切り替える
        const bool opened = sidebar_.toggle(SidebarMode::Files);
        applyLayout();
        if (opened) scrollSidebarToCurrent();
        onViewChanged();  // フィット再計算でズーム率表示が変わりうる
        break;
    }
    case Command::ToggleHelp:
        if (sidebar_.toggle(SidebarMode::Help)) {
            // ini 適用後のキーバインドから作る。開くたびに作り直すので設定変更にも追従する
            helpLines_ =
                buildHelpLines(keymap_, selectionKeymap_, mousemap_, pointer_.swapButtons());
            sidebar_.setScroll(0);
        }
        applyLayout();
        onViewChanged();  // フィット再計算でズーム率表示が変わりうる
        break;
    case Command::ToggleStatusBar:
        statusBarEnabled_ = !statusBarEnabled_;
        applyLayout();
        onViewChanged();  // フィット再計算でズーム率表示が変わりうる
        break;
    case Command::Escape:
        if (selected_) {
            selected_.reset();
            objectDrag_.end();
            host_.requestRedraw();
        } else if (drag_.dragging()) {
            drag_.end();
            host_.requestRedraw();
        } else if (origin_.edited()) {
            // 未保存の編集を抱えたまま Esc で終了してしまわないよう、まず破棄を訊く。
            // これが遷移ロックを解く既定の入口でもある(Command::DiscardEdits に既定キーは無い)
            executeDiscardEdits();
        } else if (sidebar_.showing(SidebarMode::Help)) {
            // ヘルプを見ている最中の Esc で終了してしまわないよう、まず閉じる
            execute(Command::ToggleHelp);
        } else if (host_.isFullscreen()) {
            host_.setFullscreen(false);
        } else {
            host_.quit();
        }
        break;
    case Command::Quit:
        host_.quit();
        break;
    case Command::None:
        break;
    }
}

bool App::onKey(const KeyChord& chord) {
    // 編集中はキー入力を文字編集へ回し、コマンドの暴発を防ぐ
    if (textEdit_.active()) return handleTextEditKey(chord);
    // Ctrl+B は、テキスト注釈を選択している間だけ太字トグルとして横取りする
    // (目の前で選んでいるオブジェクトへの操作を、サイドバー開閉より優先する)。
    // 編集中の Ctrl+B と同じ意味になり、選択 → 編集の行き来で挙動が変わらない
    if (chord.ctrl && !chord.shift && !chord.alt && chord.key == KeyCode{'B'} &&
        toggleSelectedTextBold()) {
        return true;
    }
    // オブジェクトを選んでいる間だけ効く表を先に引く(既定では矢印での移動)。
    // Ctrl+B の横取りと同じ「選んでいるオブジェクトへの操作を優先する」規則を、
    // 直書きの例外ではなく Keymap の層として持たせたもの。ここに無いキーは
    // 下の通常の表へ落ちるので、選択中でも Shift+矢印・PageDown などはそのまま効く
    if (selected_ && *selected_ < annotations_.size()) {
        if (const Command selectionCommand = selectionKeymap_.find(chord);
            selectionCommand != Command::None) {
            execute(selectionCommand);
            return true;
        }
    }
    const Command command = keymap_.find(chord);
    if (command == Command::None) {
        // 効くはずのキーを押して何も起きなかった = ヘルプが要る瞬間。
        // キーを覚えている利用者にはここを通らないので一生出ない
        showHelpHint();
        return false;
    }
    execute(command);
    return true;
}

void App::onResize(float width, float height) {
    clientSize_ = {width, height};
    applyLayout();
    updateTitle();  // フィット再計算でズーム率表示が変わりうる
}

void App::onWheel(float wheelNotches, Point screenPos, bool ctrl, bool shift, bool alt) {
    if (wheelNotches == 0) return;
    // 縦を回している間の横成分は誤入力とみなす(トラックボールや高精細ホイールでは
    // 縦スクロール中に微小な横が混ざり続け、放っておくと 1 段分に積み上がる)。
    // 逆向きの相殺はしない。縦を優先する非対称な扱いは意図したもの
    pointer_.resetHorizontalWheel();
    if (sidebarVisible() && screenPos.x < sidebarOffset()) {
        // サイドバー上ではズームも遷移もせず一覧をスクロール(1ノッチ = 3項目)
        sidebar_.scrollByItems(-wheelNotches * 3);
        clampSidebarScroll();
        host_.requestRedraw();
        return;
    }
    // 割り当てがあればコマンド。無ければ従来どおりカーソル位置基準のズーム
    if (wheelCommand(wheelNotches, false, ctrl, shift, alt)) return;
    pointer_.resetVerticalWheel();
    viewport_.zoomAt(std::pow(Viewport::kZoomStep, wheelNotches),
                     {screenPos.x - sidebarOffset(), screenPos.y});
    onViewChanged();
}

void App::onWheelHorizontal(float wheelNotches, Point screenPos, bool ctrl, bool shift, bool alt) {
    if (wheelNotches == 0) return;
    if (sidebarVisible() && screenPos.x < sidebarOffset()) {
        // 一覧の上では何もしない(横スクロールする中身が無い)。垂直ホイールが
        // ここでズーム・遷移をしないのと同じで、一覧を読んでいる最中に画像が
        // 切り替わらないようにする。貯金も捨てて後から効かないようにする
        pointer_.resetHorizontalWheel();
        return;
    }
    // 未割り当てのときの既定動作は無い(垂直ホイールのズームに相当するものがない)
    wheelCommand(wheelNotches, true, ctrl, shift, alt);
}

bool App::wheelCommand(float notches, bool horizontal, bool ctrl, bool shift, bool alt) {
    const MouseInput input =
        horizontal ? (notches > 0 ? MouseInput::WheelRight : MouseInput::WheelLeft)
                   : (notches > 0 ? MouseInput::WheelUp : MouseInput::WheelDown);
    const Command command = mousemap_.find({input, ctrl, shift, alt});
    if (command == Command::None) return false;
    // 1 段に達した分だけ繰り返し実行する(貯金と 1 段の重みは PointerState が持つ)
    const int steps = pointer_.wheelSteps(notches, horizontal);
    for (int i = 0; i < steps; ++i) execute(command);
    return true;
}

bool App::onMouseInput(const MouseChord& chord, Point) {
    const Command command = mousemap_.find(chord);
    if (command == Command::None) return false;
    execute(command);
    return true;
}

bool App::inViewportArea(Point screenPos) const {
    if (!current_) return false;
    const float barHeight = statusBarVisible() ? kStatusBarHeight : 0.0f;
    return screenPos.x >= sidebarOffset() && screenPos.y < clientSize_.h - barHeight;
}

bool App::onMouseDown(MouseButton button, Point screenPos) {
    pointer_.moveTo(screenPos);
    pointer_.endPan();
    pointer_.cancelMenu();
    // 右端を掴んだら幅の変更。項目のクリック判定より先に見る(境界際のクリックで
    // 画像が切り替わってしまわないように)
    if (button == MouseButton::Left && onSidebarResizeEdge(screenPos)) {
        // 見えている幅を基準に 1:1 で動かす
        sidebar_.beginResize(screenPos.x, sidebarOffset());
        return true;
    }
    // サイドバーは UI 部品なので左右の入れ替えの対象外。左クリックが項目へ移動し、
    // 右クリックは一覧のメニュー(並び替え・サブフォルダ)を開く
    if (sidebarVisible() && screenPos.x < sidebarOffset()) {
        if (button == MouseButton::Left) {
            // 掴んだファイルを覚えてから移動する(移動で一覧が入れ替わっても
            // 掴んだのは押した項目のまま。動かさずに離せば単なるクリック)
            pressSidebarItem(screenPos);
            clickSidebarItem(screenPos);
        }
        // 操作一覧モードには並べ替える一覧が無いのでメニューも出さない。
        // ここでは位置だけ覚え、ドラッグにならずに離されたら onMouseUp が開く
        if (button == MouseButton::Right && sidebar_.mode() == SidebarMode::Files) {
            pointer_.pressMenu(screenPos, true);
        }
        return true;  // サイドバー上のクリックはパン開始にしない
    }
    if (button == MouseButton::Right) {
        // メニューは入れ替えの対象外なので、役割にかかわらず右ボタンで開く。
        // ここでは位置だけ覚え、ドラッグにならずに離されたら onMouseUp が開く
        if (inViewportArea(screenPos)) pointer_.pressMenu(screenPos, false);
        // 編集中の選択範囲の上での右クリックは、確定せずに書式メニューを出す
        if (textEdit_.active() && textEdit_.index() < annotations_.size() && !isComposing() &&
            textEdit_.buffer().hasSelection()) {
            const Point imagePos = imageToScreen().inverted().apply(screenPos);
            const float tolerance = kHitTolerancePx / std::max(viewport_.zoom(), 0.001f);
            if (hitTestAnnotation(annotations_[textEdit_.index()], imagePos, tolerance)) {
                textEdit_.pressStyleMenu();  // メニューはボタンを離した位置で出す
                pointer_.cancelMenu();       // 通常のメニューは出さない
                return true;
            }
        }
    }
    // オーバーレイ矢印はサイドバーの項目と同じ UI 部品なので、入れ替えの対象外で
    // 常に左ボタン。図形を掴む判定より先に見る(端に図形があっても押せるように)
    if (button == MouseButton::Left && clickNavArrow(screenPos)) return true;
    // オブジェクトを掴む操作(選択・移動・回転・サイズ変更、テキストのキャレット移動)は
    // 入れ替えの対象外で、常に左ボタン。入れ替えると左が編集役になるが、既存の図形を
    // 選ぶのに右クリックが要るのは他のペイント系ソフトと食い違って戸惑うため
    if (button == MouseButton::Left && beginObjectGrab(screenPos)) return true;
    if (mouseRole(button) == MouseRole::Edit) {
        // 掴めなかった左ボタン(入れ替え時)と、既定の右ボタンはここへ来る
        beginEditDrag(screenPos);
        return true;
    }
    // パン役のボタンで何も掴まなかったのでドラッグはパンになる
    pointer_.beginPan();
    return false;
}

void App::clickSidebarItem(Point screenPos) {
    // ステータスバー上(サイドバー領域外)はどちらの操作でもないため何もしない
    if (textEdit_.active()) commitTextEdit();  // 画像切替の前に編集を確定する
    if (sidebar_.mode() != SidebarMode::Files || screenPos.y < 0 ||
        screenPos.y >= sidebarViewHeight()) {
        return;
    }
    const size_t index = sidebar_.itemAt(screenPos.y);
    if (index >= list_.size()) return;
    // 一覧からの選択も画像の切り替え。jumpTo は位置を書き換えるので判定を先に済ませる
    if (!guardEditLock("画像を切り替えられません")) return;
    if (list_.jumpTo(index) || origin_.fromClipboard()) refreshCurrent();
}

void App::pressSidebarItem(Point screenPos) {
    sidebarDragPath_.reset();
    if (sidebar_.mode() != SidebarMode::Files || screenPos.y < 0 ||
        screenPos.y >= sidebarViewHeight()) {
        return;  // 操作一覧にはファイルが無く、ステータスバー上は領域外
    }
    const size_t index = sidebar_.itemAt(screenPos.y);
    if (index >= list_.size()) return;
    sidebarDragPath_ = list_.at(index);
    sidebarDragPress_ = screenPos;
}

bool App::beginSidebarFileDrag(Point screenPos) {
    if (!sidebarDragPath_) return false;
    // 判定はメニューを開くかどうかと同じ閾値・同じ距離の測り方にする
    const float dx = screenPos.x - sidebarDragPress_.x;
    const float dy = screenPos.y - sidebarDragPress_.y;
    if (dx * dx + dy * dy < PointerState::kDragThresholdPx * PointerState::kDragThresholdPx) {
        return false;  // クリックとみなす範囲。まだ始めない
    }
    // 掴んだ印は先に落とす。beginFileDrag は落とされるまで返らず、対になる
    // ボタン解放の通知も来ないので、ここで畳んでおかないと次の移動でまた始まってしまう
    const fs::path path = *sidebarDragPath_;
    sidebarDragPath_.reset();
    // サイドバー上の押下はパンもメニューも始めていないので、畳む状態は他に無い
    host_.beginFileDrag({path});
    return true;
}

bool App::beginObjectGrab(Point screenPos) {
    if (!current_) return false;
    const float barHeight = statusBarVisible() ? kStatusBarHeight : 0.0f;
    if (screenPos.y >= clientSize_.h - barHeight) return false;
    objectDrag_.end();
    history_.resetDrag();
    history_.resetKeyMove();  // マウスへ持ち替えたら矢印での移動の連なりも切る
    // 選択中オブジェクトのハンドル(スクリーン座標で判定)→ 回転 / リサイズ開始
    if (selected_ && *selected_ < annotations_.size()) {
        const AnnotationSpec& spec = annotations_[*selected_];
        const Point handle = rotationHandlePos(spec, imageToScreen(), kRotationHandleOffsetPx);
        const float dx = screenPos.x - handle.x;
        const float dy = screenPos.y - handle.y;
        if (dx * dx + dy * dy <= kRotationHandleHitPx * kRotationHandleHitPx) {
            objectDrag_.beginRotate(
                spec, angleDegFrom(imageToScreen().apply(annotationCenter(spec)), screenPos));
            return true;
        }
        // ヒット領域が重なりうる(小さいオブジェクト)ため最も近いハンドルを掴む
        const std::vector<ResizeHandlePos> handles = resizeHandlePositions(spec);
        const ResizeHandlePos* nearest = nullptr;
        float bestDistSq = kResizeHandleHitPx * kResizeHandleHitPx;
        for (const ResizeHandlePos& h : handles) {
            const Point pos = imageToScreen().apply(h.pos);
            const float hx = screenPos.x - pos.x;
            const float hy = screenPos.y - pos.y;
            const float distSq = hx * hx + hy * hy;
            if (distSq <= bestDistSq) {
                bestDistSq = distSq;
                nearest = &h;
            }
        }
        if (nearest) {
            objectDrag_.beginResize(spec, nearest->handle);
            return true;
        }
    }
    const Point imagePos = imageToScreen().inverted().apply(screenPos);
    const float tolerance = kHitTolerancePx / std::max(viewport_.zoom(), 0.001f);
    // 編集中: 枠内のクリックはキャレット移動と範囲選択の開始。枠外なら確定して通常処理へ
    if (textEdit_.active() && textEdit_.index() < annotations_.size()) {
        if (hitTestAnnotation(annotations_[textEdit_.index()], imagePos, tolerance)) {
            if (isComposing()) return true;  // 変換中は位置が動かないようにする
            textEdit_.buffer().setCaret(textOffsetAt(imagePos), false);
            textEdit_.beginMouseSelect();
            notifyCaretMoved();
            host_.requestRedraw();
            return true;
        }
        commitTextEdit();
    }
    // 注釈本体 → 選択して移動ドラッグ開始。外れたら選択解除してパンに回す
    if (const auto hit = hitTestAnnotations(annotations_, imagePos, tolerance)) {
        selected_ = hit;
        objectDrag_.beginMove(imagePos, annotations_[*hit]);
        host_.requestRedraw();
        return true;
    }
    if (selected_) {
        selected_.reset();
        host_.requestRedraw();
    }
    return false;
}

void App::onMouseUp(MouseButton button, Point screenPos, bool shift) {
    pointer_.setLastScreen(screenPos);
    // 動かさずに離したのでドラッグ&ドロップにはならなかった(クリックは押下で済んでいる)
    if (button == MouseButton::Left) sidebarDragPath_.reset();
    // 書式メニューは押した時点で決まっている(編集を確定させずに出す)
    if (button == MouseButton::Right && textEdit_.consumeStyleMenu()) {
        showTextStyleMenu(screenPos);
        return;
    }
    if (button == MouseButton::Left && sidebar_.resizing()) {
        sidebar_.endResize();  // 掴んでいた間は他のドラッグを始めていない
        return;
    }
    // 掴んでいたオブジェクト操作を終える(掴むのは常に左ボタンなので解放も左だけ)
    if (button == MouseButton::Left) endObjectGrab();
    if (mouseRole(button) == MouseRole::Edit) {
        endEditDrag(screenPos, shift);  // 掴んでいたならドラッグ中でないので素通りする
    } else {
        pointer_.endPan();
    }
    // メニューは入れ替えの対象外。右ボタンをドラッグせずに離したときだけ開く
    if (button != MouseButton::Right) return;
    switch (pointer_.releaseMenu(screenPos)) {
    case MenuOnRelease::Sidebar:
        showSidebarMenu(screenPos);
        break;
    case MenuOnRelease::Pointer:
        showPointerMenu(screenPos);
        break;
    case MenuOnRelease::None:
        break;
    }
}

void App::endObjectGrab() {
    textEdit_.endMouseSelect();
    // テキストの高さは内容で決まるため、リサイズ確定時に折り返し後の実寸へ揃える。
    // 編集中は利用者が決めた枠幅を保ちたいので高さだけ合わせる
    if (objectDrag_.mode() == ObjectDragMode::Resize && history_.dragPushed() && selected_ &&
        *selected_ < annotations_.size() &&
        annotations_[*selected_].kind == AnnotationSpec::Kind::Text) {
        const bool changed = textEdit_.active() ? measureTextHeight(annotations_[*selected_])
                                                : measureTextExtent(annotations_[*selected_]);
        if (changed) {
            if (textEdit_.active()) notifyCaretMoved();
            host_.requestRedraw();
        }
    }
    objectDrag_.end();
    history_.resetDrag();
}

bool App::onDoubleClick(Point screenPos, bool ctrl, bool shift, bool alt) {
    // サイドバー上は 1 回目のクリックで画像が切り替わっているので何もしない
    if (sidebarVisible() && screenPos.x < sidebarOffset()) return false;
    if (beginTextEditByDoubleClick(screenPos)) return true;
    // テキストの再編集にならなかったダブルクリックは、割り当てがあればコマンドになる
    return onMouseInput({MouseInput::DoubleClick, ctrl, shift, alt}, screenPos);
}

bool App::beginTextEditByDoubleClick(Point screenPos) {
    if (!current_) return false;
    const Point imagePos = imageToScreen().inverted().apply(screenPos);
    const float tolerance = kHitTolerancePx / std::max(viewport_.zoom(), 0.001f);
    // 編集中の枠内でのダブルクリックは語の選択
    if (textEdit_.active() && textEdit_.index() < annotations_.size() &&
        hitTestAnnotation(annotations_[textEdit_.index()], imagePos, tolerance)) {
        if (isComposing()) return true;  // 変換中は選択を変えない
        textEdit_.buffer().selectWordAt(textOffsetAt(imagePos));
        notifyCaretMoved();
        host_.requestRedraw();
        return true;
    }
    const auto hit = hitTestAnnotations(annotations_, imagePos, tolerance);
    if (!hit || annotations_[*hit].kind != AnnotationSpec::Kind::Text) return false;
    if (textEdit_.active()) commitTextEdit();
    selected_ = hit;
    objectDrag_.end();
    beginTextEdit(*hit, {current_, annotations_, selected_}, false);
    // 文字位置はダブルクリックした場所の語を選ぶ(通常のテキスト編集と同じ)
    textEdit_.buffer().selectWordAt(textOffsetAt(imagePos));
    notifyCaretMoved();
    host_.requestRedraw();
    return true;
}

void App::beginEditDrag(Point screenPos) {
    // サイドバー・ステータスバー上、画像がないときは開始しない
    if (!inViewportArea(screenPos)) return;
    if (textEdit_.active()) commitTextEdit();  // 編集ドラッグは編集の外側なので先に確定する
    drag_.begin(screenPos, clampToImage(imageToScreen().inverted().apply(screenPos)),
                style_.penActive());
    updatePreview();
    host_.requestRedraw();
}

void App::updatePenStraightAnchor(bool shift) {
    if (!shift || !style_.penActive()) {
        drag_.resetStraightAnchor();
        return;
    }
    drag_.anchorStraight();
}

void App::extendPenPoints(float minDistancePx) {
    if (!style_.penActive()) return;
    drag_.extendPen(minDistancePx);
}

void App::updateEditDrag(Point screenPos, bool shift) {
    updatePenStraightAnchor(shift);
    drag_.setEndImage(dragEndImage(screenPos, shift));
    // 手書きは通過点を溜める(bbox ではなく軌跡そのものが図形になる)。
    // 間引きは画面上の見た目基準なので、ズームに応じて画像座標へ換算する
    extendPenPoints(kPenMinDistancePx / std::max(viewport_.zoom(), 0.001f));
    updatePreview();
    host_.requestRedraw();
}

void App::endEditDrag(Point screenPos, bool shift) {
    if (!drag_.dragging()) return;
    drag_.end();
    updatePenStraightAnchor(shift);
    drag_.setEndImage(dragEndImage(screenPos, shift));
    if (!drag_.movedEnough(screenPos, PointerState::kDragThresholdPx)) {
        // 単なるクリック(移動量が小さい)なので何も作らない。プレビューを消すだけで、
        // メニューを出すかは onMouseUp が決める(メニューは常に右クリック)
        host_.requestRedraw();
        return;
    }
    // 事前に選んであるツールをそのまま適用する(メニューは出さない)。
    // ドラッグ中のプレビューはここで実物の注釈へ置き換わる
    // 終点まで線を届かせる(同じ点なら足さない)
    extendPenPoints(0.01f);
    applyCurrentTool();
    host_.requestRedraw();
}

void App::showPointerMenu(Point screenPos) {
    // 注釈の上ならオブジェクトメニュー、そうでなければツール切り替えメニューを出す
    const Point imagePos = imageToScreen().inverted().apply(screenPos);
    const float tolerance = kHitTolerancePx / std::max(viewport_.zoom(), 0.001f);
    const auto hit = hitTestAnnotations(annotations_, imagePos, tolerance);
    if (hit) selected_ = hit;
    host_.requestRedraw();
    if (hit) {
        showObjectMenu(screenPos);
    } else {
        showToolMenu(screenPos);
    }
}

void App::showToolMenu(Point screenPos) {
    // 設定系の項目(太さ・サイズ・色)を選んだ場合はメニューを再表示し、
    // 設定を整えてからツールを選べるようにする
    while (true) {
        std::vector<EditMenuEntry> entries;
        const std::vector<MenuItem> items =
            buildEditMenu(style_, keymap_, fontAvailable(), menuImageSize(), entries);
        const auto choice = host_.showContextMenu(items, screenPos);
        if (!choice || *choice >= entries.size()) break;
        if (applyEditChoice(entries[*choice])) break;
    }
    host_.requestRedraw();
}

void App::showSidebarMenu(Point screenPos) {
    std::vector<SidebarMenuEntry> entries;
    const std::vector<MenuItem> items = buildSidebarMenu(sortOrder_, recursive_, keymap_, entries);
    const auto choice = host_.showContextMenu(items, screenPos);
    if (!choice || *choice >= entries.size()) return;
    const SidebarMenuEntry& entry = entries[*choice];
    switch (entry.action) {
    case SidebarMenuEntry::Action::SortKey:
        // メニューでは向きを反転しない(チェックの付いた項目を選び直したら現状維持)。
        // 反転は「昇順 / 降順」の項目と Command::ToggleSortDescending が担う
        setSortOrder({entry.key, entry.key == sortOrder_.key ? sortOrder_.descending
                                                            : entry.key == SortKey::Date});
        break;
    case SidebarMenuEntry::Action::SortAscending:
        setSortOrder({sortOrder_.key, false});
        break;
    case SidebarMenuEntry::Action::SortDescending:
        setSortOrder({sortOrder_.key, true});
        break;
    case SidebarMenuEntry::Action::ToggleRecursive:
        setRecursive(!recursive_);
        break;
    }
}

void App::setTool(EditTool tool) {
    style_.setTool(tool);
    host_.requestRedraw();  // ステータスバーのツール表示を更新する
}

void App::applyCurrentTool() {
    if (style_.tool() == EditTool::Ocr) {
        // 画像を変えないので、続けて別の範囲を読めるようツールは維持する
        applyOcrSelection();
        return;
    }
    applyAnnotation(kindOfTool(style_.tool()));
}

int App::nextMarkerNumber() const {
    int maxNumber = 0;
    for (const AnnotationSpec& spec : annotations_) {
        if (spec.kind == AnnotationSpec::Kind::Number) maxNumber = std::max(maxNumber, spec.number);
    }
    return maxNumber + 1;
}

FontAvailableFn App::fontAvailable() const {
    return [this](const std::string& family) { return rasterizer_.hasFontFamily(family); };
}

std::optional<MenuImageSize> App::menuImageSize() const {
    if (!current_) return std::nullopt;
    return MenuImageSize{current_->width, current_->height};
}

void App::showResizeMenu(Point screenPos) {
    if (!current_) {
        showMessage("リサイズする画像がありません");
        return;
    }
    std::vector<EditMenuEntry> entries;
    const std::vector<MenuItem> items =
        buildResizeMenu({current_->width, current_->height}, entries);
    const auto choice = host_.showContextMenu(items, screenPos);
    if (!choice || *choice >= entries.size()) return;
    applyEditChoice(entries[*choice]);
    host_.requestRedraw();
}

void App::applyResize(const uint32_t width, const uint32_t height) {
    if (!current_) {
        showMessage("リサイズする画像がありません");
        return;
    }
    if (width == current_->width && height == current_->height) return;  // 変化なし
    if (textEdit_.active()) commitTextEdit();  // 座標が変わるので先に確定させる
    auto resized = resizeImage(*current_, width, height);
    if (!resized) {
        showMessage("リサイズできませんでした(指定した大きさが大きすぎます)");
        return;
    }
    const float sx = static_cast<float>(width) / static_cast<float>(current_->width);
    const float sy = static_cast<float>(height) / static_cast<float>(current_->height);
    pushUndo();
    current_ = std::move(resized);
    // 注釈はトリミングと同じくオブジェクトのまま維持し、座標だけ倍率に追従させる
    for (AnnotationSpec& spec : annotations_) scaleAnnotation(spec, sx, sy);
    viewport_.setImage({static_cast<float>(width), static_cast<float>(height)});
    markEdited();
    host_.requestRedraw();
    showMessage(std::format("{} x {} px にリサイズしました", width, height));
}

void App::showObjectMenu(Point screenPos) {
    if (!selected_ || *selected_ >= annotations_.size()) return;
    std::vector<ObjectMenuEntry> entries;
    const std::vector<MenuItem> items = buildObjectMenu(annotations_[*selected_], keymap_,
                                                        fontAvailable(), menuImageSize(), entries);
    const auto choice = host_.showContextMenu(items, screenPos);
    if (!choice || *choice >= entries.size()) return;
    const ObjectMenuEntry entry = entries[*choice];
    AnnotationSpec& spec = annotations_[*selected_];
    switch (entry.action) {
    case ObjectMenuEntry::Action::EditText:
        beginTextEdit(*selected_, {current_, annotations_, selected_}, false);
        textEdit_.buffer().selectAll();  // 入力し直しやすいよう全選択で始める
        notifyCaretMoved();
        host_.requestRedraw();
        return;
    case ObjectMenuEntry::Action::Crop:
        cropToSelection();
        return;
    case ObjectMenuEntry::Action::Ocr:
        ocrSelectedRange();
        return;
    case ObjectMenuEntry::Action::Aspect: {
        // メニューの見出しに出した矩形をそのまま入れる。整数座標なので、次に
        // cropRectFor を通しても同じ大きさに戻り、比が丸めでずれない
        const Point p1{static_cast<float>(entry.rect.x), static_cast<float>(entry.rect.y)};
        const Point p2{static_cast<float>(entry.rect.x + entry.rect.w),
                       static_cast<float>(entry.rect.y + entry.rect.h)};
        if (spec.p1.x == p1.x && spec.p1.y == p1.y && spec.p2.x == p2.x && spec.p2.y == p2.y) {
            return;  // 既にその比・その位置なら何もしない
        }
        pushUndo();
        spec.p1 = p1;
        spec.p2 = p2;
        break;
    }
    case ObjectMenuEntry::Action::Delete:
        deleteSelectedAnnotation();
        return;
    case ObjectMenuEntry::Action::Angle:
        if (spec.angleDeg == entry.value) return;
        pushUndo();
        spec.angleDeg = entry.value;
        break;
    case ObjectMenuEntry::Action::StrokeWidth:
        if (spec.strokeWidth == entry.value) return;
        pushUndo();
        spec.strokeWidth = entry.value;
        break;
    case ObjectMenuEntry::Action::StrokeAlpha:
        if (spec.strokeAlpha == static_cast<int>(entry.value)) return;
        pushUndo();
        spec.strokeAlpha = static_cast<int>(entry.value);
        break;
    case ObjectMenuEntry::Action::Number:
        if (spec.number == static_cast<int>(entry.value)) return;
        pushUndo();
        spec.number = static_cast<int>(entry.value);
        break;
    case ObjectMenuEntry::Action::FontSize: {
        if (spec.fontSize == entry.value) return;
        // 文字サイズで実測境界が変わるため測り直す(失敗時は変更しない)
        AnnotationSpec updated = spec;
        updated.fontSize = entry.value;
        if (!measureTextExtent(updated)) {
            showMessage("描画に失敗しました");
            return;
        }
        pushUndo();
        spec = std::move(updated);
        break;
    }
    case ObjectMenuEntry::Action::FontFamily: {
        if (spec.fontFamily == entry.family) return;
        // 字幅・行の高さが変わるため、文字サイズと同じく実測し直す
        AnnotationSpec updated = spec;
        updated.fontFamily = entry.family;
        if (!measureTextExtent(updated)) {
            showMessage("描画に失敗しました");
            return;
        }
        pushUndo();
        spec = std::move(updated);
        break;
    }
    case ObjectMenuEntry::Action::PickColor: {
        const auto rgb = host_.showColorPicker(spec.colorRGB);
        if (!rgb || *rgb == spec.colorRGB) return;
        pushUndo();
        spec.colorRGB = *rgb;
        break;
    }
    case ObjectMenuEntry::Action::FillAlpha:
        if (spec.fillAlpha == static_cast<int>(entry.value)) return;
        pushUndo();
        spec.fillAlpha = static_cast<int>(entry.value);
        break;
    case ObjectMenuEntry::Action::PickFillColor: {
        const auto rgb = host_.showColorPicker(spec.fillRGB);
        if (!rgb || *rgb == spec.fillRGB) return;
        pushUndo();
        spec.fillRGB = *rgb;
        // 色だけ選んで塗られないままだと操作が空振りに見えるため、不透明で塗り始める
        if (spec.fillAlpha == 0) spec.fillAlpha = 255;
        break;
    }
    case ObjectMenuEntry::Action::BorderWidth: {
        if (spec.borderWidth == entry.value) return;
        // 枠線の太さで実測境界(余白)が変わるため測り直す
        AnnotationSpec updated = spec;
        updated.borderWidth = entry.value;
        if (!measureTextExtent(updated)) {
            showMessage("描画に失敗しました");
            return;
        }
        pushUndo();
        spec = std::move(updated);
        break;
    }
    case ObjectMenuEntry::Action::PickBorderColor: {
        const auto rgb = host_.showColorPicker(spec.borderRGB);
        if (!rgb) return;
        AnnotationSpec updated = spec;
        updated.borderRGB = *rgb;
        // 枠線なしのまま色だけ変えても見た目が変わらないため、既定の太さで引き始める
        if (updated.borderWidth <= 0) {
            updated.borderWidth = 1.0f;
            if (!measureTextExtent(updated)) {
                showMessage("描画に失敗しました");
                return;
            }
        } else if (*rgb == spec.borderRGB) {
            return;
        }
        pushUndo();
        spec = std::move(updated);
        break;
    }
    }
    markEdited();
    host_.requestRedraw();
}

void App::showTextStyleMenu(Point screenPos) {
    if (!textEdit_.active() || !textEdit_.buffer().hasSelection()) return;
    if (textEdit_.index() >= annotations_.size()) return;
    const TextStyleMenu menu =
        buildTextStyleMenu(annotations_[textEdit_.index()], textEdit_.buffer(), fontAvailable());
    const auto choice = host_.showContextMenu(menu.items, screenPos);
    bool changed = false;
    if (choice && *choice < menu.familyBase) {
        changed = textEdit_.buffer().toggleSelectionFlag(kTextStyleFlags[*choice].flag);
    } else if (choice && *choice < menu.colorIndex) {
        // 注釈全体と同じフォントを選んだら指定を外す(範囲を残さず、
        // 以降は全体のフォント変更に追従する)
        const std::string& picked = menu.families[*choice - menu.familyBase].family;
        changed = textEdit_.buffer().setSelectionFontFamily(
            picked == menu.wholeFamily ? std::string() : picked);
    } else if (choice && *choice == menu.colorIndex) {
        if (const auto rgb = host_.showColorPicker(menu.initialColor)) {
            changed = textEdit_.buffer().setSelectionColor(*rgb);
        }
    }
    if (changed) {
        applyTextEditChange();
    } else {
        host_.requestRedraw();
    }
}

bool App::applyEditChoice(const EditMenuEntry& entry) {
    switch (entry.action) {
    case EditMenuEntry::Action::SelectTool:
        setTool(entry.tool);
        return true;
    case EditMenuEntry::Action::StrokeWidth:
        style_.setStrokeWidth(entry.value);
        return false;
    case EditMenuEntry::Action::FontSize:
        style_.setFontSize(entry.value);
        return false;
    case EditMenuEntry::Action::FontFamily:
        style_.setFontFamily(entry.family);
        return false;
    case EditMenuEntry::Action::PickColor:
        if (const auto rgb = host_.showColorPicker(style_.colorRGB())) style_.setColorRGB(*rgb);
        return false;
    case EditMenuEntry::Action::FillAlpha:
        style_.setFillAlpha(static_cast<int>(entry.value));
        return false;
    case EditMenuEntry::Action::PickFillColor:
        // 塗りなしのまま色だけ選ばれても見えるよう、EditStyle 側で不透明度を起こす
        if (const auto rgb = host_.showColorPicker(style_.fillRGB())) style_.setFillColor(*rgb);
        return false;
    case EditMenuEntry::Action::BorderWidth:
        style_.setBorderWidth(entry.value);
        return false;
    case EditMenuEntry::Action::PickBorderColor:
        if (const auto rgb = host_.showColorPicker(style_.borderRGB())) style_.setBorderColor(*rgb);
        return false;
    case EditMenuEntry::Action::ResizePercent:
        if (current_) {
            const auto [w, h] = scaledSize(current_->width, current_->height, entry.value / 100.0);
            applyResize(w, h);
        }
        return true;
    case EditMenuEntry::Action::ResizeLongEdge:
        if (current_) {
            const uint32_t longest = std::max(current_->width, current_->height);
            const double factor = longest > 0 ? entry.value / longest : 1.0;
            const auto [w, h] = scaledSize(current_->width, current_->height, factor);
            applyResize(w, h);
        }
        return true;
    }
    return true;
}

const AnnotationSpec* App::selectedRangeRect() const {
    if (!current_ || !selected_ || *selected_ >= annotations_.size()) return nullptr;
    const AnnotationSpec& spec = annotations_[*selected_];
    if (spec.kind != AnnotationSpec::Kind::Rect || spec.angleDeg != 0) return nullptr;
    return &spec;
}

void App::cropToSelection() {
    if (textEdit_.active()) commitTextEdit();  // 編集を確定してから対象を確定させる
    const AnnotationSpec* range = selectedRangeRect();
    if (!range) {
        showMessage("トリミングする範囲を選んでください (回転していない矩形)");
        return;
    }
    const auto rect = cropRectFor(range->p1, range->p2, current_->width, current_->height);
    if (!rect) {
        showMessage("選択した範囲が画像の外です");
        return;
    }
    auto cropped = cropImage(*current_, *rect);
    if (!cropped) return;  // cropRectFor を通っていれば起きない
    pushUndo();
    // 範囲に使った矩形は役目を終えたので消す(残すと保存時に焼き込まれてしまう)。
    // 画像の差し替えと同じ undo 段になるので、Ctrl+Z 一発で矩形ごと戻る
    annotations_.erase(annotations_.begin() + static_cast<std::ptrdiff_t>(*selected_));
    selected_.reset();
    objectDrag_.end();
    current_ = std::move(cropped);
    // 残りの注釈はオブジェクトのまま維持し、切り出した原点ぶんだけ平行移動する
    for (AnnotationSpec& spec : annotations_) {
        translateAnnotation(spec, static_cast<float>(-rect->x), static_cast<float>(-rect->y));
    }
    viewport_.setImage(
        {static_cast<float>(current_->width), static_cast<float>(current_->height)});
    origin_.setEdited(true);
    updateTitle();
    host_.requestRedraw();
}

void App::ocrSelectedRange() {
    const AnnotationSpec* range = selectedRangeRect();
    if (!range) {
        showMessage("文字を認識する範囲を選んでください (回転していない矩形)");
        return;
    }
    const auto rect = cropRectFor(range->p1, range->p2, current_->width, current_->height);
    if (!rect) {
        showMessage("選択した範囲が画像の外です");
        return;
    }
    // 認識するのは下地の画素だけ(範囲に使った矩形の枠線は焼き込まない)
    requestOcr(cropImage(*current_, *rect));
}

void App::requestOcr(const std::shared_ptr<DecodedImage>& image) {
    if (!image) {
        showMessage("文字を認識する画像がありません");
        return;
    }
    std::shared_ptr<const DecodedImage> source = image;
    // 表示回転は認識にも効かせる(横倒しのままでは文字として認識されない)
    if (const int rotation = viewport_.rotationDegrees(); rotation != 0) {
        auto rotated = std::make_shared<DecodedImage>(*image);
        if (bakeRotation(*rotated, rotation)) source = std::move(rotated);
    }
    // 認識器はアルファを見ない。事前乗算のまま渡すと透明部分が黒になり、
    // 透過 PNG の黒い文字が背景に沈むので白へ焼き込んでおく
    if (hasTransparency(*source)) {
        if (auto flattened = flattenOnBackground(*source, 0xFFFFFF)) source = std::move(flattened);
    }
    ocrGeneration_ = ocr_.request(std::move(source));
    showMessage("文字を認識しています...");
}

bool App::applyOcrSelection() {
    if (!current_) return false;
    // トリミングと同じ丸め方(部分的にかかったピクセルも含める)
    const auto rect =
        cropRectFor(drag_.startImage(), drag_.endImage(), current_->width, current_->height);
    if (!rect) {
        showMessage("選択した範囲が画像の外です");
        return false;
    }
    requestOcr(cropImage(*current_, *rect));
    return true;
}

void App::onOcrCompleted() {
    const auto done = ocr_.takeResult();
    if (!done) return;
    if (done->generation != ocrGeneration_) return;  // 予約し直された後の古い結果
    ocrGeneration_ = 0;

    if (!done->ok) {
        showMessage(done->error.empty() ? "文字認識に失敗しました" : done->error);
        return;
    }
    const std::string text = ocrResultToText(done->result);
    if (text.empty()) {
        showMessage("文字を認識できませんでした");
        return;
    }
    if (!clipboard_.setText(text)) {
        showMessage("認識した文字のコピーに失敗しました");
        return;
    }
    showMessage(std::format("{} 行をコピーしました ({})", done->result.lines.size(),
                            done->result.language.empty() ? "文字認識" : done->result.language));
}

AnnotationSpec App::makeAnnotationSpec(AnnotationSpec::Kind kind) const {
    AnnotationSpec spec;
    spec.kind = kind;
    spec.p1 = drag_.startImage();
    spec.p2 = drag_.endImage();
    style_.applyTo(spec);  // 色・線幅・文字・塗り・枠線(マーカーの太さ・半透明も)
    if (kind == AnnotationSpec::Kind::Pen) {
        // 軌跡そのものが図形。p1/p2 は選択領域ではなく点列の bbox に合わせる
        spec.points = drag_.penPoints();
        updatePenBounds(spec);
    } else if (kind == AnnotationSpec::Kind::Number) {
        // 円の中の数字は自動で色が決まるため、色設定は円の塗りとして使う
        spec.number = nextMarkerNumber();
        spec.fillRGB = style_.colorRGB();
        spec.fillAlpha = 255;
        // 小さすぎる円は数字が潰れるだけなので、始点側を固定して最小の大きさまで広げる
        const float minSize = std::max(8.0f, style_.fontSize() * 1.8f);
        if (std::abs(spec.p2.x - spec.p1.x) < minSize) {
            spec.p2.x = spec.p1.x + (spec.p2.x < spec.p1.x ? -minSize : minSize);
        }
        if (std::abs(spec.p2.y - spec.p1.y) < minSize) {
            spec.p2.y = spec.p1.y + (spec.p2.y < spec.p1.y ? -minSize : minSize);
        }
    }
    return spec;
}

bool App::previewVisible() const {
    // テキストは中身の無い箱、文字認識は読み取る範囲で、どちらも実物を描けない。
    // その 2 つはラバーバンド(App::selection)に任せる
    return drag_.dragging() && current_ != nullptr && style_.tool() != EditTool::Text &&
           style_.tool() != EditTool::Ocr;
}

void App::updatePreview() {
    if (!previewVisible()) return;
    previewSpec_ = makeAnnotationSpec(kindOfTool(style_.tool()));
}

void App::applyAnnotation(AnnotationSpec::Kind kind) {
    if (!current_) return;
    AnnotationSpec spec = makeAnnotationSpec(kind);
    if (kind == AnnotationSpec::Kind::Text) {
        // ドラッグした矩形を空のテキストボックスにして、その場で入力を始める。
        // 内容が空のまま終われば beginTextEdit の created により削除される
        const Point origin{std::min(spec.p1.x, spec.p2.x), std::min(spec.p1.y, spec.p2.y)};
        spec.p2 = {std::max(spec.p1.x, spec.p2.x), std::max(spec.p1.y, spec.p2.y)};
        spec.p1 = origin;
        EditSnapshot before{current_, annotations_, selected_};  // 追加前の状態を undo 用に控える
        annotations_.push_back(std::move(spec));
        selected_ = annotations_.size() - 1;
        beginTextEdit(annotations_.size() - 1, std::move(before), true);
        host_.requestRedraw();
        return;
    }
    pushUndo();
    annotations_.push_back(std::move(spec));
    selected_ = annotations_.size() - 1;  // 追加直後から移動・回転できるよう選択する
    markEdited();
    host_.requestRedraw();
}

bool App::measureTextExtent(AnnotationSpec& spec) {
    // 焼き込みと同じ経路(ラスタライザ)で実測する。回転前の境界がほしいので角度は外す
    AnnotationSpec probe = spec;
    probe.angleDeg = 0;
    const AnnotationOverlay overlay = rasterizer_.rasterize(probe);
    if (!overlay.image) return false;
    const float margin = textOverlayMargin(spec);  // ラスタライザのテキスト用余白と同じ
    const float w = std::max(static_cast<float>(overlay.image->width) - margin * 2, 1.0f);
    const float h = std::max(static_cast<float>(overlay.image->height) - margin * 2, 1.0f);
    const Point origin{std::min(spec.p1.x, spec.p2.x), std::min(spec.p1.y, spec.p2.y)};
    spec.p1 = origin;
    spec.p2 = {origin.x + w, origin.y + h};
    return true;
}

bool App::measureTextHeight(AnnotationSpec& spec) {
    AnnotationSpec probe = spec;
    probe.angleDeg = 0;
    const AnnotationOverlay overlay = rasterizer_.rasterize(probe);
    if (!overlay.image) return false;
    const float margin = textOverlayMargin(spec);  // ラスタライザのテキスト用余白と同じ
    const float h = std::max(static_cast<float>(overlay.image->height) - margin * 2, 1.0f);
    spec.p2.y = std::min(spec.p1.y, spec.p2.y) + h;
    spec.p1.y = std::min(spec.p1.y, spec.p2.y);
    return true;
}

void App::beginTextEdit(size_t index, EditSnapshot before, bool created) {
    if (index >= annotations_.size()) return;
    textEdit_.begin(index, created, annotations_[index].text, annotations_[index].styles);
    history_.beginTextEdit(std::move(before));
    selected_ = index;
    objectDrag_.end();
    notifyCaretMoved();
}

void App::commitTextEdit() {
    if (!textEdit_.active()) return;
    textEdit_.end();  // 変換中なら捨てる(host 側も IME へキャンセルを通知する)
    host_.setTextEditing(false, {}, 0);
    if (textEdit_.index() < annotations_.size()) {
        AnnotationSpec& spec = annotations_[textEdit_.index()];
        spec.text = textEdit_.buffer().text();  // 変換中文字列を落とした確定内容にする
        spec.styles = textEdit_.buffer().styles();
        if (spec.text.empty()) {
            // 空のテキストボックスは残さない(新規・既存とも削除する)
            pushTextEditUndoOnce();
            annotations_.erase(annotations_.begin() +
                               static_cast<std::ptrdiff_t>(textEdit_.index()));
            selected_.reset();
            if (!textEdit_.created()) markEdited();
        } else if (!measureTextExtent(spec)) {
            showMessage("描画に失敗しました");
        }
    }
    host_.requestRedraw();
}

void App::cancelTextEdit() {
    if (!textEdit_.active()) return;
    textEdit_.end();
    host_.setTextEditing(false, {}, 0);
    // 変更を記録済みなら undo と同じ経路で編集前へ戻す。新規作成中なら追加ごと消える
    if (history_.textEditPushed()) {
        executeUndo();
        return;
    }
    if (textEdit_.created() && textEdit_.index() < annotations_.size()) {
        annotations_.erase(annotations_.begin() +
                           static_cast<std::ptrdiff_t>(textEdit_.index()));
        selected_.reset();
    }
    host_.requestRedraw();
}

void App::pushTextEditUndoOnce() {
    // 入力のたびに呼ばれる。控えてある編集前の状態を積むのは最初の 1 回だけ
    if (auto before = history_.consumeTextEditSnapshot()) pushUndoState(std::move(*before));
}

void App::applyTextEditChange() {
    if (!textEdit_.active() || textEdit_.index() >= annotations_.size()) return;
    pushTextEditUndoOnce();
    markEdited();
    refreshTextEditSpec();
}

void App::refreshTextEditSpec() {
    if (!textEdit_.active() || textEdit_.index() >= annotations_.size()) return;
    AnnotationSpec& spec = annotations_[textEdit_.index()];
    spec.text = textEdit_.displayText();
    spec.styles = textEdit_.displayStyles();
    // 空になったら枠は縮めない(利用者が決めた大きさのまま入力を続けられるように)。
    // 失敗しても枠が古いだけで編集は続けられる
    if (!spec.text.empty()) measureTextHeight(spec);
    notifyCaretMoved();
    host_.requestRedraw();
}

void App::beginComposition() {
    if (!textEdit_.active()) return;
    // 変換は選択範囲を置き換える。先に消してキャレットを 1 点にしておく
    if (textEdit_.buffer().deleteSelection()) applyTextEditChange();
}

void App::setComposition(const std::string& utf8, size_t caretBytes, size_t targetBegin,
                         size_t targetEnd) {
    if (!textEdit_.active()) return;
    textEdit_.setComposition(utf8, caretBytes, targetBegin, targetEnd);
    refreshTextEditSpec();
}

void App::clearComposition() {
    if (!textEdit_.composing()) return;
    textEdit_.resetComposition();
    refreshTextEditSpec();
}

void App::notifyCaretMoved() {
    textEdit_.showCaret();  // 移動直後は必ず見えている状態から点滅を始める
    if (!textEdit_.active() || textEdit_.index() >= annotations_.size()) return;
    const AnnotationSpec& spec = annotations_[textEdit_.index()];
    const TextCaretMetrics caret = rasterizer_.caretMetrics(
        spec, utf8ToUtf16Offset(spec.text, textEdit_.caretOffset()));
    const BoundsF bounds = annotationBounds(spec);
    // 回転後の見た目の位置へ合わせる(IME 変換ウィンドウはスクリーン座標で置く)
    const Point local{bounds.minX + caret.x, bounds.minY + caret.y};
    const Point center = annotationCenter(spec);
    const Point rotated = rotateAround(local, center, spec.angleDeg);
    const Point screen = imageToScreen().apply(rotated);
    host_.setTextEditing(true, screen, caret.height * std::max(viewport_.zoom(), 0.001f));
}

size_t App::textOffsetAt(Point imagePos) const {
    if (textEdit_.index() >= annotations_.size()) return 0;
    const AnnotationSpec& spec = annotations_[textEdit_.index()];
    const BoundsF bounds = annotationBounds(spec);
    // 注釈は中心周りに回転して描かれるため、逆回転してから枠内のローカル座標にする
    const Point unrotated = rotateAround(imagePos, annotationCenter(spec), -spec.angleDeg);
    const size_t utf16 = rasterizer_.hitTestTextOffset(spec, unrotated.x - bounds.minX,
                                                       unrotated.y - bounds.minY);
    return utf16ToUtf8Offset(textEdit_.buffer().text(), utf16);
}

void App::moveCaretVertical(bool down, bool extendSelection) {
    if (textEdit_.index() >= annotations_.size()) return;
    const AnnotationSpec& spec = annotations_[textEdit_.index()];
    TextEditBuffer& buffer = textEdit_.buffer();
    const TextCaretMetrics caret =
        rasterizer_.caretMetrics(spec, utf8ToUtf16Offset(buffer.text(), buffer.caret()));
    if (caret.height <= 0) return;
    // 現在のキャレットの 1 行上/下の中心を叩いて、その表示行の文字位置を得る
    const float y = down ? caret.y + caret.height * 1.5f : caret.y - caret.height * 0.5f;
    const size_t utf16 = rasterizer_.hitTestTextOffset(spec, caret.x, y);
    buffer.setCaret(utf16ToUtf8Offset(buffer.text(), utf16), extendSelection);
}

bool App::handleTextEditKey(const KeyChord& chord) {
    // 変換中のキーは IME が処理する(変換候補の選択・確定・取消)。App は触らない
    if (isComposing()) return true;
    const bool shift = chord.shift;
    TextEditBuffer& buffer = textEdit_.buffer();
    if (chord.ctrl && !chord.alt) {
        // 英字キーは KeyCode の列挙子ではない(ASCII をそのまま値に持つ)ため if で比べる
        const auto letter = [&chord](char c) {
            return chord.key == static_cast<KeyCode>(c);
        };
        if (chord.key == KeyCode::Enter) {
            commitTextEdit();  // Ctrl+Enter でも確定できる(Enter は改行のため)
        } else if (letter('A')) {
            buffer.selectAll();
            notifyCaretMoved();
            host_.requestRedraw();
        } else if (letter('C') || letter('X')) {
            if (buffer.hasSelection()) {
                clipboard_.setText(buffer.selectedText());
                if (letter('X')) {
                    buffer.deleteSelection();
                    applyTextEditChange();
                }
            }
        } else if (letter('V')) {
            const std::string pasted = clipboard_.getText();
            if (!pasted.empty()) {
                buffer.insert(pasted);
                applyTextEditChange();
            }
        } else if (letter('B')) {
            // 選択部分の太字トグル(全体が太字なら解除。一般的なエディタと同じ)
            if (buffer.toggleSelectionFlag(TextStyleFlag::Bold)) applyTextEditChange();
        } else if (letter('I')) {
            if (buffer.toggleSelectionFlag(TextStyleFlag::Italic)) applyTextEditChange();
        } else if (letter('U')) {
            if (buffer.toggleSelectionFlag(TextStyleFlag::Underline)) {
                applyTextEditChange();
            }
        } else if (letter('Z')) {
            cancelTextEdit();  // 入力中の取り消しは編集開始前の状態へ戻す
        }
        return true;  // 編集中の未対応 Ctrl 系もコマンドへ流さない
    }
    switch (chord.key) {
    case KeyCode::Escape:
        commitTextEdit();
        return true;
    case KeyCode::Enter:
        buffer.insert("\n");
        applyTextEditChange();
        return true;
    case KeyCode::Backspace:
        if (buffer.backspace()) applyTextEditChange();
        return true;
    case KeyCode::Delete:
        if (buffer.deleteForward()) applyTextEditChange();
        return true;
    case KeyCode::Left:
        buffer.moveLeft(shift);
        break;
    case KeyCode::Right:
        buffer.moveRight(shift);
        break;
    case KeyCode::Up:
        moveCaretVertical(false, shift);
        break;
    case KeyCode::Down:
        moveCaretVertical(true, shift);
        break;
    case KeyCode::Home:
        buffer.moveLineStart(shift);
        break;
    case KeyCode::End:
        buffer.moveLineEnd(shift);
        break;
    case KeyCode::Tab:
        buffer.insert("\t");
        applyTextEditChange();
        return true;
    default:
        return true;  // 文字キーは WM_CHAR 相当の insertText で受ける
    }
    notifyCaretMoved();
    host_.requestRedraw();
    return true;
}

void App::insertText(const std::string& utf8) {
    if (!textEdit_.active() || utf8.empty()) return;
    textEdit_.resetComposition();  // 確定文字列が変換中文字列を置き換える
    textEdit_.buffer().insert(utf8);
    applyTextEditChange();
}

bool App::wantsTextCursor(Point screenPos) const {
    if (!textEdit_.active() || textEdit_.index() >= annotations_.size()) return false;
    const Point imagePos = imageToScreen().inverted().apply(screenPos);
    const float tolerance = kHitTolerancePx / std::max(viewport_.zoom(), 0.001f);
    return hitTestAnnotation(annotations_[textEdit_.index()], imagePos, tolerance);
}

void App::onCaretBlink() {
    if (!textEdit_.active()) return;
    textEdit_.blinkCaret();
    host_.requestRedraw();
}

bool App::toggleSelectedTextBold() {
    if (!selected_ || *selected_ >= annotations_.size()) return false;
    AnnotationSpec& spec = annotations_[*selected_];
    if (spec.kind != AnnotationSpec::Kind::Text || spec.text.empty()) return false;
    // 全体が太字なら解除、そうでなければ全体を太字に(編集中の Ctrl+B と同じ規則)
    const bool bold = isTextStyleFlagSet(spec.styles, 0, spec.text.size(), TextStyleFlag::Bold);
    AnnotationSpec updated = spec;
    setTextStyleFlag(updated.styles, 0, updated.text.size(), TextStyleFlag::Bold, !bold);
    // 太さで字幅・行の高さが変わるため、フォント変更と同じく実測し直す
    if (!measureTextExtent(updated)) {
        showMessage("描画に失敗しました");
        return true;  // 横取りはした(サイドバーが開いてしまわないように)
    }
    pushUndo();
    spec = std::move(updated);
    markEdited();
    host_.requestRedraw();
    return true;
}

void App::deleteSelectedAnnotation() {
    if (textEdit_.active()) commitTextEdit();  // 編集を確定してから対象を確定させる
    if (!selected_ || *selected_ >= annotations_.size()) {
        showMessage("削除する注釈がありません");
        return;
    }
    pushUndo();
    annotations_.erase(annotations_.begin() + static_cast<std::ptrdiff_t>(*selected_));
    selected_.reset();
    objectDrag_.end();
    markEdited();
    host_.requestRedraw();
}

void App::moveSelectedObject(const Point screenDelta) {
    if (!selected_ || *selected_ >= annotations_.size()) return;
    // 移動量は画像 1px 固定(ズームで刻みが変わらない)。向きだけ表示回転を打ち消す
    const Point delta = screenNudgeToImage(screenDelta, viewport_.rotationDegrees());
    // 連続した移動は 1 段にまとめる。1 打ごとに積むと上限 10 段を数回で使い切り、
    // それ以前の編集が取り消せなくなる(まとめてあるので Ctrl+Z 一発で移動前に戻る)
    if (history_.consumeKeyMovePush()) pushUndo();
    translateAnnotation(annotations_[*selected_], delta.x, delta.y);
    markEdited();
    host_.requestRedraw();
}

bool App::executePasteImage() {
    // 表示中の画像を置き換える = 切り替えと同じなのでロックの対象。
    // 注釈として貼る PasteObject は編集そのものなので断らない
    if (!guardEditLock("画像を貼り付けられません")) return false;
    auto image = clipboard_.getImage();
    if (!image || image->width == 0 || image->height == 0) {
        showMessage("クリップボードに画像がありません");
        return false;
    }
    discardEdits();
    resetSequence();  // 貼り付け画像は 1 枚きり(再生中なら止まる)
    current_ = std::move(image);
    origin_.setClipboard();
    viewport_.setImage(
        {static_cast<float>(current_->width), static_cast<float>(current_->height)});
    updateTitle();
    host_.requestRedraw();
    return true;
}

void App::executePasteObject() {
    // 下地が無い / この環境では注釈を扱えない場合は、画像そのものの貼り付けにする
    // (置いても見えず保存もされないオブジェクトを作らない)
    if (!current_ || !rasterizer_.available()) {
        const bool unsupported = current_ && !rasterizer_.available();
        if (executePasteImage() && unsupported) {
            showMessage("この環境では画像オブジェクトを扱えないため、画像として開きました");
        }
        return;
    }
    auto image = clipboard_.getImage();
    if (!image || image->width == 0 || image->height == 0) {
        showMessage("クリップボードに画像がありません");
        return;
    }
    // 焼き込み時のオーバーレイはラスタライザの上限までしか作れないので、
    // 取り込む時点で縮めておく(あとで縮小しても画素は戻らないため、ここが唯一の機会)
    if (image->width > kMaxResizeDimension || image->height > kMaxResizeDimension) {
        auto reduced = downscaleToFit(*image, kMaxResizeDimension);
        if (!reduced) {
            showMessage("貼り付けられませんでした(画像が大きすぎます)");
            return;
        }
        image = std::move(reduced);
    }
    if (textEdit_.active()) commitTextEdit();  // 編集を確定してから新しい選択へ移る

    // 可視領域の中心へ置く(サイドバー・ステータスバーを除いた矩形の中心)
    const float barHeight = statusBarVisible() ? kStatusBarHeight : 0.0f;
    const Point viewCenter{(sidebarOffset() + clientSize_.w) * 0.5f,
                           (clientSize_.h - barHeight) * 0.5f};
    const BoundsF box = pastedImageBounds(
        {static_cast<float>(image->width), static_cast<float>(image->height)},
        {static_cast<float>(current_->width), static_cast<float>(current_->height)},
        imageToScreen().inverted().apply(viewCenter));

    const uint32_t pastedWidth = image->width;
    const uint32_t pastedHeight = image->height;
    pushUndo();
    AnnotationSpec spec;
    spec.kind = AnnotationSpec::Kind::Image;
    spec.p1 = {box.minX, box.minY};
    spec.p2 = {box.maxX, box.maxY};
    spec.image = std::move(image);
    annotations_.push_back(std::move(spec));
    selected_ = annotations_.size() - 1;  // 貼った直後から移動・リサイズできるよう選択する
    markEdited();
    host_.requestRedraw();
    showMessage(std::format("{} x {} px の画像を貼り付けました", pastedWidth, pastedHeight));
}

void App::executeSaveOverwrite() {
    if (!current_) {
        showMessage("保存する画像がありません");
        return;
    }
    // 貼り付け画像には上書き先のファイルが無いので、保存先を尋ねる方へ回す
    if (origin_.fromClipboard() || list_.empty()) {
        executeSaveAs();
        return;
    }
    // 表示中の 1 枚で上書きすると、残りのページ / フレームが消える。縮小して取り込んだ
    // 画像と同じく、確認ダイアログを出す前に断る(名前を付けて保存は許す)
    if (multiFrame()) {
        showMessage(std::format(
            "この画像は{}が {} 枚あります。上書きすると表示中の 1 枚だけになるので、"
            "名前を付けて保存を使ってください",
            frameUnitLabel(), sequence_->frames.size()));
        return;
    }
    if (const std::string reason = overwriteBlockedReason(*current_); !reason.empty()) {
        showMessage(reason);  // 確認ダイアログを出す前に断る
        return;
    }
    const fs::path path = list_.current();
    if (!encoder_.supports(path)) {
        showMessage(std::format("{} は上書き保存に対応していない形式です"
                                "(PNG / JPEG / BMP のみ)。名前を付けて保存を使ってください",
                                pathToUtf8(path.extension())));
        return;
    }
    // カラープロファイル付きの画像は、上書きすると色の情報が失われる。
    // 変換済みなら元の色空間が、変換前(遅延変換の待ち中)ならプロファイルが消える
    std::string colorWarning;
    if (current_->colorConverted) {
        colorWarning =
            "\nこの画像は埋め込みプロファイルから sRGB へ変換して表示しているため、"
            "上書きすると元の色空間の情報は失われます。";
    } else if (current_->colorPending) {
        colorWarning =
            "\nこの画像の埋め込みカラープロファイルは保存時に引き継がれないため、"
            "上書きすると色の解釈が変わります。";
    }
    if (confirmOverwrite_ &&
        !host_.showConfirm(std::format("{} を上書き保存します。\n"
                                       "元の画像は元に戻せません。{}よろしいですか?",
                                       pathToUtf8(path.filename()), colorWarning))) {
        return;
    }
    saveImageTo(path, true);
}

void App::executeSaveAs() {
    if (!current_) {
        showMessage("保存する画像がありません");
        return;
    }
    const std::string defaultName = origin_.fromClipboard() || list_.empty()
                                        ? "クリップボード.png"
                                        : pathToUtf8(list_.current().stem()) + ".png";
    if (const auto path = host_.showSaveDialog(defaultName)) {
        // 一覧の現在のファイルを選び直した場合も上書きなので同じ後始末をする
        const bool isOverwrite =
            !origin_.fromClipboard() && !list_.empty() && samePath(*path, list_.current());
        saveImageTo(*path, isOverwrite);
    }
}

void App::executePrint() {
    if (!current_) {
        showMessage("印刷する画像がありません");
        return;
    }
    // 印刷するのは画面で見えているとおりのもの(注釈と表示回転を焼き込む)
    std::shared_ptr<const DecodedImage> image = compositeImage();
    // 紙は白。事前乗算のまま渡すと透明部分が黒くなるので焼き込んでおく
    // (GDI はアルファを見ない。文字認識へ渡すときと同じ理由)
    if (hasTransparency(*image)) {
        if (auto flattened = flattenOnBackground(*image, 0xFFFFFF)) image = std::move(flattened);
    }
    const std::string jobName = origin_.fromClipboard() || list_.empty()
                                    ? "クリップボードの画像"
                                    : pathToUtf8(list_.current().filename());
    switch (printer_.print(*image, jobName, printOptions_)) {
    case PrintStatus::Printed:
        showMessage("印刷しました: " + jobName);
        break;
    case PrintStatus::Canceled:
        break;  // 利用者が取りやめただけなので何も出さない(保存ダイアログと同じ)
    case PrintStatus::Unsupported:
        showMessage("このビルドでは印刷に対応していません");
        break;
    case PrintStatus::Failed:
        showMessage("印刷に失敗しました");
        break;
    }
}

void App::saveImageTo(const fs::path& path, const bool isOverwrite) {
    // 名前を付けて保存で現在のファイルを選び直した場合もここへ来るので、同じ理由で断る
    if (isOverwrite && current_) {
        if (const std::string reason = overwriteBlockedReason(*current_); !reason.empty()) {
            showMessage(reason);
            return;
        }
    }
    if (!encoder_.encode(*compositeImage(), path, encodeOptions_)) {
        showMessage("保存に失敗しました: " + pathToUtf8(path));
        return;
    }
    // 表示のために変えてある点(縮小・色変換)は保存結果にも入るので伝える
    const bool reduced = current_ && current_->downscaled();
    const bool converted = current_ && current_->colorConverted;
    std::string note;
    if (reduced && converted) {
        note = std::format("(表示用に縮小した {} x {} / sRGB へ変換した色で)", current_->width,
                           current_->height);
    } else if (reduced) {
        note = std::format("(表示用に縮小した {} x {} で)", current_->width, current_->height);
    } else if (converted) {
        note = "(sRGB へ変換した色で)";
    }
    showMessage(std::format("{}しました{}: {}", isOverwrite ? "上書き保存" : "保存", note,
                            pathToUtf8(path)));
    if (!isOverwrite) return;
    // ディスクの内容が表示に一致したので、未保存マークを消してキャッシュを捨てる
    // (捨てないと戻ってきたときに保存前のピクセルが出る)
    cache_.invalidate(path);
    if (origin_.setEdited(false)) updateTitle();
}

std::shared_ptr<DecodedImage> App::compositeImage() const {
    const int rotation = viewport_.rotationDegrees();
    if (!current_ || (annotations_.empty() && rotation == 0)) return current_;
    auto out = std::make_shared<DecodedImage>(*current_);  // キャッシュ共有のためコピー
    for (const AnnotationSpec& spec : annotations_) {
        const AnnotationOverlay overlay = rasterizer_.rasterize(spec);
        if (overlay.image) blendOverlay(*out, *overlay.image, overlay.x, overlay.y);
    }
    // 注釈は画像座標なので、焼き込んでから回す(画面で見えているとおりに出る)
    bakeRotation(*out, rotation);
    return out;
}

std::shared_ptr<DecodedImage> App::selectedObjectImage() const {
    if (!selected_ || *selected_ >= annotations_.size()) return nullptr;
    AnnotationOverlay overlay = rasterizer_.rasterize(annotations_[*selected_]);
    if (!overlay.image) return nullptr;
    // 注釈は画像座標なので、compositeImage と同じく表示回転は最後に焼き込む
    // (画面で見えているとおりの向きで貼り付けられる)
    bakeRotation(*overlay.image, viewport_.rotationDegrees());
    return std::move(overlay.image);
}

void App::executeCopyImage() {
    // オブジェクトを選んでいる間はそれだけをコピーする。選択中は操作の対象が
    // オブジェクトなので、Ctrl+C の対象もそちらに合わせる(下地ごと欲しいときは
    // Esc で選択を外す)
    if (selected_ && *selected_ < annotations_.size()) {
        const std::shared_ptr<DecodedImage> object = selectedObjectImage();
        if (object && clipboard_.setImage(*object)) {
            showMessage("オブジェクトをクリップボードにコピーしました");
        } else {
            showMessage("オブジェクトのコピーに失敗しました");
        }
        return;
    }
    if (!current_) {
        showMessage("コピーする画像がありません");
    } else if (clipboard_.setImage(*compositeImage())) {
        showMessage("画像をクリップボードにコピーしました");
    } else {
        showMessage("画像のコピーに失敗しました");
    }
}

void App::markEdited() {
    if (origin_.setEdited(true)) updateTitle();
}

void App::pushUndo() {
    pushUndoState({current_, annotations_, selected_});
}

void App::pushUndoState(EditSnapshot state) {
    // 編集を始めたらアニメーションは止める。以後は「そのフレームの静止画」を触っている
    // ことになる(再生を続けると編集した画素が次のフレームで消えてしまう)
    stopPlayback();
    history_.push(std::move(state));
}

void App::pushDragUndoOnce() {
    // ドラッグ中は変更のたびに呼ばれる。スナップショットを作るのは最初の 1 回だけ
    if (!history_.consumeDragPush()) return;
    pushUndo();
}

void App::executeUndo() {
    if (textEdit_.active()) commitTextEdit();  // 編集中の内容を確定してから履歴を戻す
    if (!restoreFrom(history_.undo({current_, annotations_, selected_}))) {
        showMessage("取り消す編集はありません");
        return;
    }
    // 履歴を使い切った = 開いた直後の状態に戻った
    origin_.setEdited(history_.canUndo());
    updateTitle();
    host_.requestRedraw();
}

void App::executeRedo() {
    if (textEdit_.active()) commitTextEdit();
    if (!restoreFrom(history_.redo({current_, annotations_, selected_}))) {
        showMessage("やり直す編集はありません");
        return;
    }
    origin_.setEdited(true);  // やり直した先は必ず何らかの編集が入った状態
    updateTitle();
    host_.requestRedraw();
}

bool App::restoreFrom(std::optional<EditSnapshot> state) {
    if (!state) return false;
    const bool sizeChanged =
        current_ && state->image &&
        (current_->width != state->image->width || current_->height != state->image->height);
    current_ = std::move(state->image);
    annotations_ = std::move(state->annotations);
    // 選択も控えてある(移動を取り消したときに対象が選択されたまま残るように)。
    // index はこのスナップショットの注釈一覧に対するものだが、念のため範囲を確かめる
    selected_ = state->selected;
    if (selected_ && *selected_ >= annotations_.size()) selected_.reset();
    objectDrag_.end();
    // トリミングの取り消しでサイズが戻るときだけビューを再設定する(回転等を保つ)
    if (sizeChanged) {
        viewport_.setImage(
            {static_cast<float>(current_->width), static_cast<float>(current_->height)});
    }
    return true;
}

void App::discardEdits() {
    if (textEdit_.active()) {
        // 画像が変わるため注釈ごと捨てられる。host へ編集終了だけ伝えて状態を落とす
        textEdit_.end();
        host_.setTextEditing(false, {}, 0);
    }
    drag_.reset();
    selected_.reset();
    objectDrag_.end();
    annotations_.clear();
    if (history_.empty() && !origin_.edited()) return;
    history_.clear();
    if (origin_.setEdited(false)) showMessage("編集を破棄しました");
}

bool App::editLocked() const {
    return lockNavigation_ && origin_.edited();
}

std::string App::editLockHint() const {
    std::string hint;
    const auto add = [&](const std::string& keys, const std::string_view label) {
        if (keys.empty()) return;  // ini で外されているキーは案内しない
        if (!hint.empty()) hint += " / ";
        hint += std::format("{} {}", keys, label);
    };
    add(keysLabel(keymap_, Command::SaveImage), "保存");
    // 破棄の既定の入口は Esc の連鎖。直接の割り当てがあればそちらを優先して案内する
    std::string discard = keysLabel(keymap_, Command::DiscardEdits);
    if (discard.empty()) discard = keysLabel(keymap_, Command::Escape);
    add(discard, "破棄");
    add(keysLabel(keymap_, Command::Undo), "取り消し");
    return hint;
}

bool App::guardEditLock(const std::string_view what) {
    if (!editLocked()) return true;
    const std::string hint = editLockHint();
    showMessage(hint.empty() ? std::format("編集中は{}", what)
                             : std::format("編集中は{}({})", what, hint));
    return false;
}

bool App::confirmEditLock(const std::string_view what) {
    if (!editLocked()) return true;
    return host_.showConfirm(
        std::format("{}\n未保存の編集は破棄されます。よろしいですか?", what));
}

bool App::executeDiscardEdits() {
    if (!origin_.edited()) {
        showMessage("破棄する編集はありません");
        return false;
    }
    // 破棄した編集は undo でも戻せないので、ロックの有無によらず必ず確認する
    if (!host_.showConfirm("編集内容をすべて破棄します。\n"
                           "元に戻すことはできません。よろしいですか?")) {
        return false;
    }
    // 注釈だけでなくトリミング・リサイズしたピクセルまで戻す必要があるので、
    // 読み直せるならファイルから読み直す(画像を送って戻ってきたときと同じ状態になる)
    if (!origin_.fromClipboard() && !list_.empty()) {
        refreshCurrent();  // 先頭の discardEdits が注釈と履歴を捨て、通知も出す
        return true;
    }
    // 貼り付け画像には読み直す元が無いので、履歴を遡れるところまで遡って戻す
    // (ビューの作り直しは restoreFrom が大きさの変わった段でだけ行う)
    while (history_.canUndo()) restoreFrom(history_.undo({current_, annotations_, selected_}));
    discardEdits();
    updateTitle();
    host_.requestRedraw();
    return true;
}

Point App::clampToImage(Point imagePos) const {
    if (!current_) return imagePos;
    return {std::clamp(imagePos.x, 0.0f, static_cast<float>(current_->width)),
            std::clamp(imagePos.y, 0.0f, static_cast<float>(current_->height))};
}

Point App::dragEndImage(Point screenPos, bool shift) const {
    const Point p = clampToImage(imageToScreen().inverted().apply(screenPos));
    // 連番マーカーは常に円にしたいので、Shift の有無によらず正方形へ寄せる
    if (style_.tool() == EditTool::Number) return constrainToSquare(drag_.startImage(), p);
    if (!shift) return p;
    // 手書きは軌跡そのものが図形なので、選択領域の形ではなく線の向きを揃える。
    // 起点は Shift を押した時点の点(それまでに描いた軌跡はそのまま残る)
    if (style_.penActive()) {
        const auto anchor = drag_.straightAnchorPoint();
        return anchor ? constrainToAxis(*anchor, p) : p;
    }
    // 直線・矢印は bbox ではなく線の向きを揃えたいので、正方形化(=45 度固定)
    // ではなく水平・垂直・45 度へのスナップにする
    if (style_.tool() == EditTool::Line || style_.tool() == EditTool::Arrow) {
        return constrainToAxis(drag_.startImage(), p);
    }
    return constrainToSquare(drag_.startImage(), p);
}

SelectionView App::selection() const {
    SelectionView sel;
    // 図形ツールはプレビューで実物を描くので、ラバーバンドは出さない
    sel.visible = drag_.dragging() && current_ != nullptr && !previewVisible();
    if (!sel.visible) return sel;
    const Matrix3x2 m = imageToScreen();
    sel.p1 = m.apply(drag_.startImage());
    sel.p2 = m.apply(drag_.endImage());
    sel.borderRGB = 0x3399FF;
    sel.fillARGB = 0x303399FF;  // 半透明の塗り
    return sel;
}

// --- オーバーレイ矢印(廃止しうる表示。判定の幾何は core/nav_arrows.h) ---------

NavArrowsState App::navArrowsGeometry() const {
    if (!navArrowsEnabled_ || !pointer_.inside() || list_.empty()) return {};
    if (editLocked()) return {};  // 押しても遷移できないボタンは出さない
    // ドラッグ中とテキスト編集中は出さない(操作の途中で押せてしまうと編集が消える)
    if (pointer_.panning() || drag_.dragging() || sidebar_.resizing() || textEdit_.active() ||
        textEdit_.mouseSelecting() || objectDrag_.active()) {
        return {};
    }
    const float offset = sidebarOffset();
    const float barHeight = statusBarVisible() ? kStatusBarHeight : 0.0f;
    const SizeF viewport{clientSize_.w - offset, clientSize_.h - barHeight};
    // 貼り付け画像の表示中は、前後どちらでもフォルダ一覧の表示へ戻れる
    const bool hasPrev = origin_.fromClipboard() || list_.index() > 0;
    const bool hasNext = origin_.fromClipboard() || list_.index() + 1 < list_.size();
    const Point pointer = pointer_.lastScreen();
    NavArrowsState state =
        navArrowsState(viewport, Point{pointer.x - offset, pointer.y}, hasPrev, hasNext);
    // ビューポート左上原点 → スクリーン座標
    for (NavArrow* arrow : {&state.prev, &state.next}) {
        arrow->p1.x += offset;
        arrow->p2.x += offset;
    }
    return state;
}

NavArrowsView App::navArrows() const {
    NavArrowsView view;
    view.arrows = navArrowsGeometry();
    // 画像の上に重ねるので、テーマ(darkTheme_)ではなく明暗どちらの画像でも
    // 見えるように固定色にする
    view.backgroundRGB = 0x000000;
    view.alpha = 96;
    view.hoverAlpha = 176;
    view.glyphRGB = 0xFFFFFF;
    return view;
}

bool App::clickNavArrow(Point screenPos) {
    const auto next = hitTestNavArrows(navArrowsGeometry(), screenPos);
    if (!next) return false;
    execute(*next ? Command::NextImage : Command::PrevImage);
    return true;
}

bool App::updateNavArrowHover() {
    const NavArrowsState state = navArrowsGeometry();
    const auto same = [](const NavArrow& a, const NavArrow& b) {
        return a.visible == b.visible && a.hovered == b.hovered;
    };
    if (same(state.prev, navArrowsShown_.prev) && same(state.next, navArrowsShown_.next)) {
        return false;
    }
    navArrowsShown_ = state;
    return true;
}

AnnotationsView App::annotations() const {
    AnnotationsView view;
    view.specs = &annotations_;
    if (selected_ && *selected_ < annotations_.size()) view.selected = selected_;
    view.selectionRGB = 0x3399FF;
    view.handleOffsetPx = kRotationHandleOffsetPx;
    view.handleRadiusPx = kRotationHandleRadiusPx;
    view.resizeHandleSizePx = kResizeHandleSizePx;
    if (previewVisible()) view.preview = &previewSpec_;
    if (textEdit_.active() && textEdit_.index() < annotations_.size()) {
        const AnnotationSpec& spec = annotations_[textEdit_.index()];
        const BoundsF bounds = annotationBounds(spec);
        TextEditView& edit = view.textEdit;
        edit.active = true;
        edit.index = textEdit_.index();
        edit.caretVisible = textEdit_.caretOn() && !textEdit_.buffer().hasSelection();
        // spec.text は変換中文字列を混ぜた表示用テキスト。位置指定はこれを基準にする
        const std::string& text = spec.text;
        const TextCaretMetrics caret =
            rasterizer_.caretMetrics(spec, utf8ToUtf16Offset(text, textEdit_.caretOffset()));
        // レンダラは注釈と同じ変換で描くため、枠原点を足した画像座標で渡す
        edit.caretTop = {bounds.minX + caret.x, bounds.minY + caret.y};
        edit.caretBottom = {edit.caretTop.x, edit.caretTop.y + caret.height};
        // 枠原点ぶんずらして画像座標へ直す
        const auto toImageRects = [&bounds](std::vector<TextRangeRect> rects) {
            for (TextRangeRect& r : rects) {
                r.left += bounds.minX;
                r.right += bounds.minX;
                r.top += bounds.minY;
                r.bottom += bounds.minY;
            }
            return rects;
        };
        const auto rectsFor = [&](size_t beginBytes, size_t endBytes) {
            return toImageRects(rasterizer_.selectionRects(spec,
                                                           utf8ToUtf16Offset(text, beginBytes),
                                                           utf8ToUtf16Offset(text, endBytes)));
        };
        const TextEditBuffer& buffer = textEdit_.buffer();
        if (buffer.hasSelection()) {
            edit.selectionRects = rectsFor(buffer.selectionBegin(), buffer.selectionEnd());
        }
        if (textEdit_.composing()) {
            const size_t base = buffer.caret();  // 変換中文字列はここに挿入されている
            edit.compositionRects = rectsFor(base, base + textEdit_.composition().size());
            if (textEdit_.compositionTargetEnd() > textEdit_.compositionTargetBegin()) {
                edit.compositionTargetRects =
                    rectsFor(base + textEdit_.compositionTargetBegin(),
                             base + textEdit_.compositionTargetEnd());
            }
        }
        edit.caretRGB = 0x3399FF;
        edit.selectionARGB = 0x603399FF;
    }
    return view;
}

void App::panBy(float dx, float dy) {
    viewport_.panBy(dx, dy);
    host_.requestRedraw();
}

bool App::onShiftChanged(bool shift) {
    if (drag_.dragging()) {
        updateEditDrag(pointer_.lastScreen(), shift);
        return true;
    }
    // オブジェクトのハンドルを掴んでいる間も同じ(端点スナップ・回転スナップが追従する)。
    // 位置は変わらないので、移動量 0 の onMouseMove として処理すればよい
    if (objectDrag_.active()) {
        onMouseMove(pointer_.lastScreen(), shift);
        return true;
    }
    return false;
}

void App::onMouseMove(Point screenPos, bool shift) {
    // オーバーレイ矢印の表示判定もここで立つ(onMouseLeave で false へ戻す)
    const Point delta = pointer_.moveTo(screenPos);
    // 幅の変更中は掴んだ位置からの総移動量で決める(クランプで取りこぼしが出ないように)
    if (sidebar_.resizing()) {
        setSidebarWidth(sidebar_.resizeWidth(screenPos.x));
        return;
    }
    // サイドバーの項目を掴んだまま動かしたら、ファイルを他のアプリへ渡す
    if (beginSidebarFileDrag(screenPos)) return;
    // パン役のボタンで何も掴まずにドラッグしている間は画像を動かす
    if (pointer_.panning()) panBy(delta.x, delta.y);
    // 編集ドラッグ中は選択領域とプレビューを更新する(ホバー表示の更新も続ける)
    if (drag_.dragging()) updateEditDrag(screenPos, shift);
    // テキスト編集中のドラッグは範囲選択(キャレット側だけを動かす)
    if (textEdit_.mouseSelecting()) {
        const Point imagePos = imageToScreen().inverted().apply(screenPos);
        textEdit_.buffer().setCaret(textOffsetAt(imagePos), true);
        notifyCaretMoved();
        host_.requestRedraw();
        return;
    }
    // 注釈の移動・回転ドラッグ中はホバー表示より優先する
    if (objectDrag_.active() && selected_ && *selected_ < annotations_.size()) {
        AnnotationSpec& spec = annotations_[*selected_];
        const AnnotationSpec& orig = objectDrag_.origSpec();
        if (objectDrag_.mode() == ObjectDragMode::Move) {
            const Point imagePos = imageToScreen().inverted().apply(screenPos);
            const auto [dx, dy] = objectDrag_.moveDelta(imagePos);
            pushDragUndoOnce();
            spec.p1 = {orig.p1.x + dx, orig.p1.y + dy};
            spec.p2 = {orig.p2.x + dx, orig.p2.y + dy};
            // 手書きは点列が実体なので bbox と一緒に動かす
            spec.points = orig.points;
            for (Point& p : spec.points) {
                p.x += dx;
                p.y += dy;
            }
        } else if (objectDrag_.mode() == ObjectDragMode::Rotate) {
            const Point center = imageToScreen().apply(annotationCenter(spec));
            float angle = objectDrag_.rotatedAngleDeg(angleDegFrom(center, screenPos));
            if (shift) angle = snapAngleDeg(angle, kAngleSnapDeg);
            pushDragUndoOnce();
            spec.angleDeg = normalizeAngleDeg(angle);
        } else {
            const Point imagePos = imageToScreen().inverted().apply(screenPos);
            pushDragUndoOnce();
            AnnotationSpec resized = resizeAnnotation(orig, objectDrag_.handle(), imagePos,
                                                      resizeKeepsAspect(orig, shift));
            spec.p1 = resized.p1;
            spec.p2 = resized.p2;
            spec.points = std::move(resized.points);  // 手書きは点列も拡縮されている
        }
        markEdited();
        host_.requestRedraw();
        return;
    }
    // 表示が変わるときだけ再描画する(オーバーレイ矢印の出入りとホバーもここで拾う)
    bool redraw = updateNavArrowHover();
    std::string text = hoverInfoText(screenPos);
    if (text != hoverText_) {
        hoverText_ = std::move(text);
        redraw = redraw || statusBarVisible();
    }
    if (redraw) host_.requestRedraw();
}

void App::onMouseLeave() {
    pointer_.setInside(false);  // ウィンドウから出たらオーバーレイ矢印を消す
    bool redraw = updateNavArrowHover();
    if (!hoverText_.empty()) {
        hoverText_.clear();
        redraw = redraw || statusBarVisible();
    }
    if (redraw) host_.requestRedraw();
}

void App::onTimer() {
    if (message_.empty()) return;
    message_.clear();
    if (statusBarVisible()) host_.requestRedraw();
}

void App::onDecodeCompleted() {
    if (origin_.fromClipboard()) return;  // 貼り付け画像の表示はデコード完了で上書きしない
    if (origin_.edited()) return;         // 編集中の画像も同様
    if (list_.empty()) return;
    // 表示すべき画像がまだ画面に出ていなければ取得を再試行する
    if (origin_.path() == list_.current() && (current_ || origin_.failed())) {
        adoptRefinedImage();  // 表示中の画像が良い版に差し替わっていれば拾う
        adoptSequence();      // 調査・展開で増えたフレームがあれば拾う
        return;
    }
    refreshCurrent();
}

void App::onFrameTimer() {
    if (!playback_.playing || !sequence_) return;
    // ini の loop = true(既定)ならファイルのループ回数を無視して回し続ける
    const int loops = animationOptions_.loopForever ? 0 : sequence_->loopCount;
    if (advanceFrame(playback_, sequence_->frames.size(), loops)) {
        showFrame(playback_.index);
        scheduleFrameTimer();
        return;
    }
    // 繰り返しが尽きた(advanceFrame が再生を止めた)。最後のフレームを出したまま停止する
    host_.setFrameTimer(0);
    host_.requestRedraw();
}

void App::adoptRefinedImage() {
    if (!current_ || origin_.path().empty()) return;
    // 多フレームの画像では tryGet が返すのは先頭フレーム。別のフレームを表示している
    // ときに拾うと絵が飛ぶので、フレーム列は adoptSequence に任せる
    if (multiFrame()) return;
    auto latest = cache_.tryGet(origin_.path());
    if (!latest || latest == current_) return;
    // 大きさが変わる差し替えは来ない想定だが、来たら座標系が食い違うので拒む
    if (latest->width != current_->width || latest->height != current_->height) return;
    current_ = std::move(latest);
    host_.requestRedraw();  // ズーム・パン・注釈はそのまま(画素だけ入れ替わる)
}

void App::adoptSequence() {
    if (origin_.path().empty() || origin_.fromClipboard() || origin_.edited()) return;
    auto latest = cache_.tryGetSequence(origin_.path());
    if (!latest || latest == sequence_) return;
    const size_t before = sequence_ ? sequence_->frames.size() : 0;
    sequence_ = std::move(latest);
    if (playback_.index >= sequence_->frames.size()) playback_.index = 0;
    if (sequence_->truncated) {
        showMessage("フレーム数が多いため、先頭のフレームだけを静止画として表示しています");
    }
    // 展開後の先頭フレームは論理画面いっぱいに合成されていて、デコード直後のもの
    // (GIF の先頭フレームは画面より小さいことがある)と大きさが違いうる
    showFrame(playback_.index);
    if (sequence_->frames.size() != before) {
        if (animationOptions_.autoplay) setPlaying(true);  // アニメでなければ何もしない
        host_.requestRedraw();  // ステータスバーのフレーム数表示
    }
}

void App::resetSequence() {
    stopPlayback();
    sequence_.reset();
    playback_ = {};
}

void App::stopPlayback() {
    if (!playback_.playing) return;
    playback_.playing = false;
    host_.setFrameTimer(0);
}

void App::setPlaying(const bool play) {
    if (play && !(sequence_ && sequence_->kind == SequenceKind::Animation && multiFrame())) {
        return;  // アニメーションでないものは再生しない
    }
    if (playback_.playing == play) return;
    playback_.playing = play;
    if (play) {
        playback_.loopsDone = 0;
        scheduleFrameTimer();
    } else {
        host_.setFrameTimer(0);
    }
    host_.requestRedraw();  // ステータスバーの「再生中 / 停止中」表示
}

void App::scheduleFrameTimer() {
    if (!playback_.playing || !sequence_ || playback_.index >= sequence_->frames.size()) return;
    host_.setFrameTimer(normalizedDelayMs(sequence_->frames[playback_.index].delayMs,
                                          animationOptions_.minDelayMs,
                                          animationOptions_.defaultDelayMs));
}

void App::showFrame(const size_t index) {
    if (!sequence_ || index >= sequence_->frames.size()) return;
    playback_.index = index;
    const std::shared_ptr<DecodedImage>& image = sequence_->frames[index].image;
    if (!image) {
        // 未デコードのページ。読み込みを頼み、前のフレームを出したまま待つ
        cache_.requestFrame(origin_.path(), static_cast<uint32_t>(index));
        host_.requestRedraw();  // ステータスバーのページ番号だけ先に進む
        return;
    }
    if (current_ == image) return;
    // ページごとに大きさが違う場合(ICO のサイズ違いなど)だけフィットし直す。
    // アニメーションは全フレームが同じ論理画面なのでズーム・パンが保たれる
    const bool sizeChanged =
        !current_ || current_->width != image->width || current_->height != image->height;
    current_ = image;
    if (sizeChanged) {
        viewport_.setImage(
            {static_cast<float>(current_->width), static_cast<float>(current_->height)});
    }
    updateTitle();
    host_.requestRedraw();
}

bool App::multiFrame() const { return sequence_ && sequence_->frames.size() > 1; }

std::string_view App::frameUnitLabel() const {
    return sequence_ && sequence_->kind == SequenceKind::Animation ? "フレーム" : "ページ";
}

void App::executeTogglePlay() {
    if (!sequence_ || sequence_->kind != SequenceKind::Animation || !multiFrame()) {
        showMessage("この画像はアニメーションではありません");
        return;
    }
    if (origin_.edited()) {
        showMessage("編集中は再生できません(元に戻すと再生できます)");
        return;
    }
    setPlaying(!playback_.playing);
}

void App::stepFrame(const int delta) {
    if (!multiFrame()) {
        showMessage("この画像は 1 フレームしかありません");
        return;
    }
    const size_t count = sequence_->frames.size();
    if (delta > 0 && playback_.index + 1 >= count) {
        showMessage(std::format("最後の{}です", frameUnitLabel()));
        return;
    }
    if (delta < 0 && playback_.index == 0) {
        showMessage(std::format("最初の{}です", frameUnitLabel()));
        return;
    }
    // フレーム切替も編集を捨てるので、画像切替と同じくロックの対象にする
    if (!guardEditLock(std::format("{}を送れません", frameUnitLabel()))) return;
    setPlaying(false);  // 手で送ったら一時停止する(送った先を見たいはずなので)
    discardEdits();     // フレーム切替は画像切替と同じ扱いにする(規則を増やさない)
    showFrame(delta > 0 ? playback_.index + 1 : playback_.index - 1);
    host_.requestRedraw();
}

void App::navigate(bool moved) {
    // 一覧が空なら戻る先が無い。ここで refreshCurrent を呼ぶと貼り付け画像を捨てた末に
    // 何も表示できず、二度と戻せなくなる(画像を読まずに起動して貼り付けた場合)
    if (list_.empty()) return;
    // 貼り付け画像の表示中は、一覧位置が動かなくてもフォルダ一覧の表示へ戻す
    if (moved || origin_.fromClipboard()) refreshCurrent();
}

void App::refreshCurrent() {
    discardEdits();
    resetSequence();  // 前の画像のフレーム列と再生状態を捨てる
    // ここから先はどの枝を通っても表示がフォルダ一覧由来に戻る(貼り付けの印が落ちる)
    if (list_.empty()) {
        current_.reset();
        origin_.clear();
        updateTitle();
        host_.requestRedraw();
        return;
    }
    const fs::path& path = list_.current();
    bool failed = false;
    std::string error;
    if (auto image = cache_.tryGet(path, &failed, &error)) {
        current_ = std::move(image);
        origin_.setFile(path);
        viewport_.setImage(
            {static_cast<float>(current_->width), static_cast<float>(current_->height)});
        // 表示に採用した時点でフレーム構成を調べる(先読みでは調べていない)。
        // 既に調査・展開済みならその場で受け取り、まだなら完了通知で adoptSequence が拾う
        cache_.requestSequence(path);
        sequence_ = cache_.tryGetSequence(path);
        if (multiFrame()) {
            showFrame(0);
            if (animationOptions_.autoplay) setPlaying(true);
        }
    } else if (failed) {
        current_.reset();
        origin_.setFailed(path, std::move(error));
    } else {
        // デコード待ち。前の画像を表示したまま onDecodeCompleted を待つ
        cache_.requestNow(path);
        origin_.setLoading();
    }
    scrollSidebarToCurrent();
    updatePrefetch();
    updateTitle();
    host_.requestRedraw();
}

void App::onViewChanged() {
    updateTitle();
    host_.requestRedraw();
}

bool App::statusBarVisible() const {
    return statusBarEnabled_ && !host_.isFullscreen();
}

bool App::sidebarVisible() const {
    return sidebar_.enabled() && !host_.isFullscreen();
}

float App::sidebarOffset() const {
    return sidebarVisible() ? sidebar_.width() : 0.0f;
}

void App::setSidebarWidth(float width) {
    if (!sidebar_.setWidth(width, clientSize_.w)) return;
    applyLayout();
    onViewChanged();  // フィット再計算でズーム率表示が変わりうる
}

bool App::onSidebarResizeEdge(Point screenPos) const {
    if (!sidebarVisible()) return false;
    if (screenPos.y < 0 || screenPos.y >= sidebarViewHeight()) return false;
    return sidebar_.onResizeEdge(screenPos.x);
}

bool App::wantsSidebarResizeCursor(Point screenPos) const {
    return sidebar_.resizing() || onSidebarResizeEdge(screenPos);
}

size_t App::sidebarItemCount() const {
    return sidebar_.mode() == SidebarMode::Help ? helpLines_.size() : list_.size();
}

float App::sidebarViewHeight() const {
    const float barHeight = statusBarVisible() ? kStatusBarHeight : 0.0f;
    return std::max(clientSize_.h - barHeight, 1.0f);
}

void App::clampSidebarScroll() {
    sidebar_.clampScroll(sidebarItemCount(), sidebarViewHeight());
}

void App::scrollSidebarToCurrent() {
    if (sidebar_.mode() == SidebarMode::Help) return;  // 操作一覧に「現在項目」はない
    if (list_.empty()) {
        sidebar_.setScroll(0);
        return;
    }
    sidebar_.scrollToItem(list_.index(), list_.size(), sidebarViewHeight());
}

void App::applyLayout() {
    const float barHeight = statusBarVisible() ? kStatusBarHeight : 0.0f;
    viewport_.setWindowSize({std::max(clientSize_.w - sidebarOffset(), 1.0f),
                             std::max(clientSize_.h - barHeight, 1.0f)});
    clampSidebarScroll();
}

Matrix3x2 App::imageToScreen() const {
    // ビューポートはサイドバーの右側から始まる
    return viewport_.imageToScreen() * Matrix3x2::translation(sidebarOffset(), 0);
}

std::string App::hoverInfoText(Point screenPos) const {
    if (!current_) return {};
    // ビューポート外(サイドバー・ステータスバー上を含む)では表示しない
    const float barHeight = statusBarVisible() ? kStatusBarHeight : 0.0f;
    if (screenPos.x < sidebarOffset() || screenPos.x >= clientSize_.w || screenPos.y < 0 ||
        screenPos.y >= clientSize_.h - barHeight) {
        return {};
    }
    const Point p = imageToScreen().inverted().apply(screenPos);
    return pixelInfoText(*current_, static_cast<int>(std::floor(p.x)),
                         static_cast<int>(std::floor(p.y)));
}

void App::showMessage(std::string text) {
    message_ = std::move(text);
    host_.startTimer(kMessageDurationMs);
    if (statusBarVisible()) host_.requestRedraw();
}

StatusBarView App::statusBar() const {
    StatusBarView bar;
    bar.visible = statusBarVisible();
    if (!bar.visible) return bar;
    bar.height = kStatusBarHeight;
    bar.backgroundRGB = darkTheme_ ? 0x2B2B2B : 0xF2F2F2;
    bar.textRGB = darkTheme_ ? 0xD8D8D8 : 0x202020;
    StatusTextState state;
    state.message = message_;
    state.image = current_.get();
    state.frame = frameStatus();
    state.error = origin_.error();
    state.tool = style_.tool();
    state.recursive = recursive_;
    state.failed = origin_.failed();
    state.editLocked = editLocked();
    bar.leftText = statusText(state);
    bar.rightText = hoverText_;
    // 寸法を合わせる手がかりは他に無いので、選択中は大きさを常に見せる
    if (std::string size = selectionSizeText(); !size.empty()) {
        if (!bar.rightText.empty()) size += "  |  ";
        bar.rightText = std::move(size) + bar.rightText;
    }
    return bar;
}

std::string App::selectionSizeText() const {
    if (!selected_ || *selected_ >= annotations_.size()) return {};
    // 範囲として使える矩形は、実際に切り出される大きさ(メニューの見出しと同じ値)を出す
    if (const AnnotationSpec* range = selectedRangeRect()) {
        const auto rect = cropRectFor(range->p1, range->p2, current_->width, current_->height);
        return rect ? objectSizeText(rect->w, rect->h) : std::string();
    }
    const BoundsF bounds = annotationBounds(annotations_[*selected_]);
    return objectSizeText(static_cast<int>(std::lround(bounds.maxX - bounds.minX)),
                          static_cast<int>(std::lround(bounds.maxY - bounds.minY)));
}

std::optional<FrameStatus> App::frameStatus() const {
    if (!multiFrame()) return std::nullopt;
    FrameStatus frame;
    frame.unitLabel = frameUnitLabel();
    frame.label = sequence_->frames[playback_.index].label;
    frame.index = playback_.index;
    frame.count = sequence_->frames.size();
    frame.animation = sequence_->kind == SequenceKind::Animation;
    frame.playing = playback_.playing;
    return frame;
}

SidebarView App::sidebar() const {
    SidebarView sb;
    sb.visible = sidebarVisible();
    if (!sb.visible) return sb;
    sb.width = sidebarOffset();
    sb.height = sidebarViewHeight();
    sb.itemHeight = SidebarState::kItemHeight;
    sb.backgroundRGB = darkTheme_ ? 0x252526 : 0xF3F3F3;
    sb.textRGB = darkTheme_ ? 0xCCCCCC : 0x333333;
    sb.currentBackgroundRGB = darkTheme_ ? 0x094771 : 0xCCE4F7;
    sb.currentTextRGB = darkTheme_ ? 0xFFFFFF : 0x1A1A1A;
    sb.scrollbarRGB = darkTheme_ ? 0x666666 : 0xA0A0A0;
    sb.scrollOffset = sidebar_.scroll();
    const size_t count = sidebarItemCount();
    sb.contentHeight = static_cast<float>(count) * SidebarState::kItemHeight;

    // 可視範囲の項目だけを渡す(先頭が部分的に隠れる分は firstItemY が負になる)
    const size_t first = sidebar_.firstVisibleItem();
    sb.firstItemY = sidebar_.firstItemY();
    const size_t maxVisible = static_cast<size_t>(sb.height / SidebarState::kItemHeight) + 2;
    for (size_t i = first; i < count && i < first + maxVisible; ++i) {
        if (sidebar_.mode() == SidebarMode::Help) {
            // 見出し行を current の強調表示で描く(レンダラは両モードを区別しない)
            sb.items.push_back({helpLines_[i].text, helpLines_[i].header});
        } else {
            sb.items.push_back({sidebarLabel(i), i == list_.index()});
        }
    }
    return sb;
}

std::string App::sidebarLabel(const size_t index) const {
    // 再帰中はファイル名だけだと別フォルダの同名・連番が区別できないので相対パスを出す
    if (recursive_ && index < order_.size() && order_[index] < entries_.size()) {
        return pathToUtf8(entries_[order_[index]].relative);
    }
    return pathToUtf8(list_.at(index).filename());
}

void App::updatePrefetch() {
    cache_.setPrefetch(list_.prefetchOrder(prefetchRadius_));
}

void App::updateTitle() {
    const std::string appName = std::format("Blinker v{} ({})", kAppVersion, kAppGitSha);
    // ファイル名は string_view で渡すので、組み立てが終わるまで実体を保つ
    const std::string filename =
        list_.empty() ? std::string() : pathToUtf8(list_.current().filename());
    TitleState state;
    state.appName = appName;
    state.filename = filename;
    state.index = list_.empty() ? 0 : list_.index();
    state.count = list_.size();
    state.zoom = viewport_.zoom();
    state.fromClipboard = origin_.fromClipboard();
    state.failed = origin_.failed();
    state.loading = !list_.empty() && origin_.path() != list_.current();
    state.edited = origin_.edited();
    host_.setTitle(windowTitle(state));
}

} // namespace blinker
