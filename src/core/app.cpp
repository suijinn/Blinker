#include "core/app.h"

#include <algorithm>
#include <array>
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

// 区切り線のメニュー項目({.separator = true} は gcc の
// -Wmissing-field-initializers 警告になるため関数にする)
MenuItem menuSeparator() {
    MenuItem item;
    item.separator = true;
    return item;
}

// 塗りつぶしの不透明度の選択肢(0-255)。0 は塗らない = 完全な透過
constexpr std::array<int, 5> kFillAlphaChoices{0, 64, 128, 191, 255};
// テキストの枠線幅の選択肢(画面px基準)。0 は枠線なし
constexpr std::array<int, 6> kBorderWidthChoices{0, 1, 2, 3, 5, 8};

std::string fillAlphaLabel(int alpha) {
    if (alpha <= 0) return "なし (透明)";
    return std::format("{}%", std::lround(alpha * 100.0 / 255.0));
}

std::string borderWidthLabel(int width) {
    return width <= 0 ? std::string("なし") : std::format("{}px", width);
}

// フォントの選択肢。{表示ラベル, 描画側へ渡すファミリ名}。和文ゴシック・明朝・
// UD・欧文・等幅を一通り並べてある。入っていないものはメニューに出さない
constexpr std::array<std::pair<const char*, const char*>, 9> kFontFamilyChoices{{
    {"游ゴシック", "Yu Gothic"},
    {"游ゴシック UI", "Yu Gothic UI"},
    {"游明朝", "Yu Mincho"},
    {"メイリオ", "Meiryo"},
    {"BIZ UDPゴシック", "BIZ UDPGothic"},
    {"MS ゴシック", "MS Gothic"},
    {"MS 明朝", "MS Mincho"},
    {"Segoe UI", "Segoe UI"},
    {"Consolas", "Consolas"},
}};

// 実際に描画に使われるフォント名。未指定(空)は既定フォントとして扱う。
// std::string を渡しても一時オブジェクトを作らないよう、引数・戻り値とも view で通す
std::string_view effectiveFontFamily(std::string_view family) {
    return family.empty() ? std::string_view(kDefaultFontFamily) : family;
}

// メニューの見出しに出す名前。候補表にあれば日本語ラベル、無ければファミリ名のまま
std::string fontFamilyLabel(std::string_view family) {
    const std::string_view name = effectiveFontFamily(family);
    for (const auto& [label, value] : kFontFamilyChoices) {
        if (name == value) return label;
    }
    return std::string(name);
}

// ツールの表示名(メニューとステータスバーで共通)
std::string_view toolLabel(EditTool tool) {
    switch (tool) {
    case EditTool::Crop:    return "トリミング";
    case EditTool::Rect:    return "矩形";
    case EditTool::Ellipse: return "楕円";
    case EditTool::Arrow:   return "矢印";
    case EditTool::Line:    return "直線";
    case EditTool::Pen:     return "ペン (手書き)";
    case EditTool::Marker:  return "マーカー";
    case EditTool::Number:  return "連番マーカー";
    case EditTool::Text:    return "テキスト";
    case EditTool::Ocr:     return "文字認識";
    }
    return "";
}

// blinker.ini の [edit] tool = に書く名前。[keys] の tool_* コマンド名と揃える
std::optional<EditTool> toolFromName(std::string_view name) {
    const std::string lower = toLower(trim(name));
    if (lower == "crop") return EditTool::Crop;
    if (lower == "rect") return EditTool::Rect;
    if (lower == "ellipse") return EditTool::Ellipse;
    if (lower == "arrow") return EditTool::Arrow;
    if (lower == "line") return EditTool::Line;
    if (lower == "pen") return EditTool::Pen;
    if (lower == "marker") return EditTool::Marker;
    if (lower == "number") return EditTool::Number;
    if (lower == "text") return EditTool::Text;
    if (lower == "ocr") return EditTool::Ocr;
    return std::nullopt;
}

// ツール切り替えに対応するコマンド。メニュー項目にキー表記を出すために使う
Command commandOfTool(EditTool tool) {
    switch (tool) {
    case EditTool::Crop:    return Command::SelectToolCrop;
    case EditTool::Rect:    return Command::SelectToolRect;
    case EditTool::Ellipse: return Command::SelectToolEllipse;
    case EditTool::Arrow:   return Command::SelectToolArrow;
    case EditTool::Line:    return Command::SelectToolLine;
    case EditTool::Pen:     return Command::SelectToolPen;
    case EditTool::Marker:  return Command::SelectToolMarker;
    case EditTool::Number:  return Command::SelectToolNumber;
    case EditTool::Text:    return Command::SelectToolText;
    case EditTool::Ocr:     return Command::SelectToolOcr;
    }
    return Command::None;
}

// 並び替えキーに対応するコマンド。メニュー項目にキー表記を出すために使う
Command commandOfSortKey(SortKey key) {
    switch (key) {
    case SortKey::Name:      return Command::SortByName;
    case SortKey::Date:      return Command::SortByDate;
    case SortKey::Size:      return Command::SortBySize;
    case SortKey::Extension: return Command::SortByExtension;
    }
    return Command::None;
}

// 縦横比を保ったまま倍率を掛けた大きさ。0 にならないよう最低 1px は残す
std::pair<uint32_t, uint32_t> scaledSize(uint32_t width, uint32_t height, double factor) {
    const auto scale = [factor](const uint32_t v) {
        return std::max(1u, static_cast<uint32_t>(std::lround(v * factor)));
    };
    return {scale(width), scale(height)};
}

// 図形ツールが作る注釈の種別。Crop は注釈ではないので Rect を返す(呼ばれない)。
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
    case EditTool::Crop:
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
    keymap_.applyConfig(config.section("keys"));
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
    editColorRGB_ = config.getColorRGB("edit", "color", editColorRGB_);
    editStrokeWidth_ = static_cast<float>(std::clamp(
        config.getInt("edit", "stroke_width", static_cast<int>(editStrokeWidth_)), 1, 100));
    editFontSize_ = static_cast<float>(
        std::clamp(config.getInt("edit", "font_size", static_cast<int>(editFontSize_)), 6, 200));
    // フォントは候補表に無い名前も受け付ける(入っていなければ描画側がフォールバックする)。
    // 存在確認はしない(起動時にシステムフォントを列挙したくないため)
    if (const std::string family{trim(config.get("edit", "font_family"))}; !family.empty()) {
        editFontFamily_ = family;
    }
    editFillRGB_ = config.getColorRGB("edit", "fill_color", editFillRGB_);
    editFillAlpha_ = std::clamp(config.getInt("edit", "fill_alpha", editFillAlpha_), 0, 255);
    editBorderRGB_ = config.getColorRGB("edit", "border_color", editBorderRGB_);
    editBorderWidth_ = static_cast<float>(std::clamp(
        config.getInt("edit", "border_width", static_cast<int>(editBorderWidth_)), 0, 100));
    // 起動時の(編集ドラッグで実行される)ツール。未指定・未知の名前なら既定のまま
    if (const auto tool = toolFromName(config.get("edit", "tool"))) setTool(*tool);
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
    displayedPath_.clear();
    loadFailed_ = false;
    loadError_.clear();
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
    // 位置の正は「一覧の現在位置」で、表示中の画像 (displayedPath_) ではない。
    // デコード待ちの間は両者がずれており、displayedPath_ を基準にすると
    // 進行中の画像送りが取り消されてしまう
    const fs::path keep = list_.empty() ? fs::path{} : list_.current();
    // 貼り付け画像の表示中は一覧の位置に意味が無いので先頭へ寄せる(表示はそのまま)
    applyListOrder(clipboardImage_ ? fs::path{} : keep);
    // 求めた位置に着けなかった(一覧が空だった・現在の画像が消えた)ときだけ表示を
    // 切り替える。着けたなら refreshCurrent は呼ばない — 編集中の内容を捨ててしまうため
    if (!clipboardImage_ && !list_.empty() && !samePath(list_.current(), keep)) {
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
    switch (command) {
    case Command::NextImage:
        navigate(list_.next());
        break;
    case Command::PrevImage:
        navigate(list_.prev());
        break;
    case Command::FirstImage:
        navigate(list_.first());
        break;
    case Command::LastImage:
        navigate(list_.last());
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
        if (clipboardImage_ || list_.empty()) {
            showMessage("コピーするパスがありません");
        } else if (clipboard_.setText(pathToUtf8(list_.current()))) {
            showMessage("パスをコピーしました: " + pathToUtf8(list_.current()));
        } else {
            showMessage("パスのコピーに失敗しました");
        }
        break;
    case Command::CopyFile:
        // コピーされるのはディスク上の元ファイル(未保存の編集内容は含まれない)
        if (clipboardImage_ || list_.empty()) {
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
    case Command::DeleteAnnotation:
        deleteSelectedAnnotation();
        break;
    case Command::SelectToolCrop:
        setTool(EditTool::Crop);
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
            helpLines_ = buildHelpLines(keymap_, mousemap_, pointer_.swapButtons());
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
        if (button == MouseButton::Left) clickSidebarItem(screenPos);
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
    if (index < list_.size() && (list_.jumpTo(index) || clipboardImage_)) {
        refreshCurrent();
    }
}

bool App::beginObjectGrab(Point screenPos) {
    if (!current_) return false;
    const float barHeight = statusBarVisible() ? kStatusBarHeight : 0.0f;
    if (screenPos.y >= clientSize_.h - barHeight) return false;
    objectDrag_.end();
    history_.resetDrag();
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
    beginTextEdit(*hit, {current_, annotations_}, false);
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
                penToolActive());
    updatePreview();
    host_.requestRedraw();
}

void App::updatePenStraightAnchor(bool shift) {
    if (!shift || !penToolActive()) {
        drag_.resetStraightAnchor();
        return;
    }
    drag_.anchorStraight();
}

void App::extendPenPoints(float minDistancePx) {
    if (!penToolActive()) return;
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
        const std::vector<MenuItem> items = buildEditMenu(entries);
        const auto choice = host_.showContextMenu(items, screenPos);
        if (!choice || *choice >= entries.size()) break;
        if (applyEditChoice(entries[*choice])) break;
    }
    host_.requestRedraw();
}

std::vector<MenuItem> App::buildSidebarMenu(std::vector<SidebarMenuEntry>& entries) const {
    const auto leaf = [&entries](std::string text, SidebarMenuEntry entry,
                                 const bool checked = false) {
        entries.push_back(entry);
        MenuItem item;
        item.text = std::move(text);
        item.checked = checked;
        return item;
    };
    using Action = SidebarMenuEntry::Action;
    const auto sortKey = [&leaf, this](const std::string_view label, const SortKey key) {
        std::string text(label);
        if (const std::string keys = keysLabel(keymap_, commandOfSortKey(key)); !keys.empty()) {
            text += '\t';
            text += keys;
        }
        return leaf(std::move(text), {Action::SortKey, key}, key == sortOrder_.key);
    };

    std::vector<MenuItem> items;
    MenuItem sort;
    sort.text = std::format("並び替え ({})", sortOrderLabel(sortOrder_));
    sort.children.push_back(sortKey("名前", SortKey::Name));
    sort.children.push_back(sortKey("更新日時", SortKey::Date));
    sort.children.push_back(sortKey("サイズ", SortKey::Size));
    sort.children.push_back(sortKey("種類", SortKey::Extension));
    items.push_back(std::move(sort));
    items.push_back(menuSeparator());
    items.push_back(leaf("昇順", {Action::SortAscending}, !sortOrder_.descending));
    items.push_back(leaf("降順", {Action::SortDescending}, sortOrder_.descending));
    items.push_back(menuSeparator());

    std::string recursive = "サブフォルダを含める";
    if (const std::string keys = keysLabel(keymap_, Command::ToggleRecursive); !keys.empty()) {
        recursive += '\t';
        recursive += keys;
    }
    items.push_back(leaf(std::move(recursive), {Action::ToggleRecursive}, recursive_));
    return items;
}

void App::showSidebarMenu(Point screenPos) {
    std::vector<SidebarMenuEntry> entries;
    const std::vector<MenuItem> items = buildSidebarMenu(entries);
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
    // トリミングは一度きりなので、戻り先として直前の図形ツールを覚えておく
    if (tool != EditTool::Crop) toolAfterCrop_ = tool;
    tool_ = tool;
    host_.requestRedraw();  // ステータスバーのツール表示を更新する
}

void App::applyCurrentTool() {
    if (tool_ == EditTool::Crop) {
        if (applyCrop()) setTool(toolAfterCrop_);  // 切り出せたら図形ツールへ戻る
        return;
    }
    if (tool_ == EditTool::Ocr) {
        // トリミングと違い画像を変えないので、続けて別の範囲を読めるようツールは維持する
        applyOcrSelection();
        return;
    }
    applyAnnotation(kindOfTool(tool_));
}

bool App::penToolActive() const {
    return tool_ == EditTool::Pen || tool_ == EditTool::Marker;
}

int App::nextMarkerNumber() const {
    int maxNumber = 0;
    for (const AnnotationSpec& spec : annotations_) {
        if (spec.kind == AnnotationSpec::Kind::Number) maxNumber = std::max(maxNumber, spec.number);
    }
    return maxNumber + 1;
}

std::vector<std::pair<std::string, std::string>> App::fontFamilyChoices(
    std::string_view current) const {
    const std::string_view effective = effectiveFontFamily(current);
    std::vector<std::pair<std::string, std::string>> choices;
    bool listed = false;
    for (const auto& [label, family] : kFontFamilyChoices) {
        if (effective == family) {
            listed = true;  // 現在のフォントは、入っていなくても選び直せるよう必ず出す
        } else if (!rasterizer_.hasFontFamily(family)) {
            continue;
        }
        choices.emplace_back(label, family);
    }
    if (!listed) choices.emplace_back(effective, effective);
    return choices;
}

std::vector<MenuItem> App::buildEditMenu(std::vector<EditMenuEntry>& entries) const {
    const auto leaf = [&entries](std::string text, EditMenuEntry entry, bool checked = false) {
        entries.push_back(entry);
        MenuItem item;
        item.text = std::move(text);
        item.checked = checked;
        return item;
    };
    // ツールは選ぶだけで、実際の適用は次の編集ドラッグで行う。現在のツールにチェックが付く。
    // ini でキーを割り当てていれば、'\t' 区切りで右寄せのアクセラレータ表記として出る
    const auto tool = [&leaf, this](EditTool t) {
        std::string text(toolLabel(t));
        if (const std::string keys = keysLabel(keymap_, commandOfTool(t)); !keys.empty()) {
            text += '\t';
            text += keys;
        }
        return leaf(std::move(text), {EditMenuEntry::Action::SelectTool, t, 0}, t == tool_);
    };
    using Action = EditMenuEntry::Action;

    std::vector<MenuItem> items;
    items.push_back(tool(EditTool::Crop));
    items.push_back(tool(EditTool::Ocr));
    items.push_back(menuSeparator());
    items.push_back(tool(EditTool::Rect));
    items.push_back(tool(EditTool::Ellipse));
    items.push_back(tool(EditTool::Arrow));
    items.push_back(tool(EditTool::Line));
    items.push_back(tool(EditTool::Pen));
    items.push_back(tool(EditTool::Marker));
    items.push_back(tool(EditTool::Number));
    items.push_back(tool(EditTool::Text));
    items.push_back(menuSeparator());

    MenuItem stroke;
    stroke.text = std::format("線の太さ ({}px)", static_cast<int>(editStrokeWidth_));
    for (const int w : {1, 2, 3, 5, 8, 12, 20}) {
        stroke.children.push_back(
            leaf(std::format("{}px", w),
                 {Action::StrokeWidth, EditTool::Rect, static_cast<float>(w)},
                 static_cast<float>(w) == editStrokeWidth_));
    }
    items.push_back(std::move(stroke));

    MenuItem font;
    font.text = std::format("文字サイズ ({}px)", static_cast<int>(editFontSize_));
    for (const int s : {12, 14, 18, 24, 36, 48, 72}) {
        font.children.push_back(
            leaf(std::format("{}px", s),
                 {Action::FontSize, EditTool::Rect, static_cast<float>(s)},
                 static_cast<float>(s) == editFontSize_));
    }
    items.push_back(std::move(font));

    MenuItem family;
    family.text = std::format("フォント ({})", fontFamilyLabel(editFontFamily_));
    for (const auto& [label, name] : fontFamilyChoices(editFontFamily_)) {
        family.children.push_back(
            leaf(label, {Action::FontFamily, EditTool::Rect, 0, name}, name == editFontFamily_));
    }
    items.push_back(std::move(family));

    items.push_back(
        leaf(std::format("色の変更... (#{:06X})", editColorRGB_), {Action::PickColor}));

    // 塗りつぶし(矩形・楕円・テキスト)。不透明度 0 で塗らない = 背景が透ける
    MenuItem fill;
    fill.text = std::format("塗りつぶし ({})", fillAlphaLabel(editFillAlpha_));
    for (const int a : kFillAlphaChoices) {
        fill.children.push_back(
            leaf(fillAlphaLabel(a), {Action::FillAlpha, EditTool::Rect, static_cast<float>(a)},
                 a == editFillAlpha_));
    }
    fill.children.push_back(menuSeparator());
    fill.children.push_back(leaf(std::format("色の変更... (#{:06X})", editFillRGB_),
                                 {Action::PickFillColor}));
    items.push_back(std::move(fill));

    // 枠線はテキストボックス用(矩形・楕円の輪郭は「線の太さ」「色の変更」で指定する)
    MenuItem border;
    border.text = std::format("テキストの枠線 ({})",
                              borderWidthLabel(static_cast<int>(editBorderWidth_)));
    for (const int w : kBorderWidthChoices) {
        border.children.push_back(
            leaf(borderWidthLabel(w),
                 {Action::BorderWidth, EditTool::Rect, static_cast<float>(w)},
                 static_cast<float>(w) == editBorderWidth_));
    }
    border.children.push_back(menuSeparator());
    border.children.push_back(leaf(std::format("色の変更... (#{:06X})", editBorderRGB_),
                                   {Action::PickBorderColor}));
    items.push_back(std::move(border));

    // リサイズはツールではなく一度きりの操作だが、画像に対する操作の入口がこのメニュー
    // しかないのでここへ置く(Command::ResizeImage は同じ内容を単独で出す)
    if (current_) {
        items.push_back(menuSeparator());
        MenuItem resize;
        resize.text = std::format("画像をリサイズ ({} x {})", current_->width, current_->height);
        resize.children = buildResizeMenu(entries);
        items.push_back(std::move(resize));
    }
    return items;
}

std::vector<MenuItem> App::buildResizeMenu(std::vector<EditMenuEntry>& entries) const {
    const auto leaf = [&entries](std::string text, EditMenuEntry entry) {
        entries.push_back(entry);
        MenuItem item;
        item.text = std::move(text);
        return item;
    };
    using Action = EditMenuEntry::Action;
    const uint32_t width = current_ ? current_->width : 0;
    const uint32_t height = current_ ? current_->height : 0;
    // 変換後の大きさを項目に添える(選ぶ前に結果が分かるように)
    const auto label = [width, height](const std::string& head, const double factor) {
        const auto [w, h] = scaledSize(width, height, factor);
        return std::format("{}\t{} x {}", head, w, h);
    };

    std::vector<MenuItem> items;
    MenuItem percent;
    percent.text = "倍率";
    for (const int p : {200, 150, 75, 50, 25}) {
        percent.children.push_back(
            leaf(label(std::format("{}%", p), p / 100.0),
                 {Action::ResizePercent, EditTool::Rect, static_cast<float>(p)}));
    }
    items.push_back(std::move(percent));

    MenuItem longEdge;
    longEdge.text = "長辺を指定";
    const uint32_t longest = std::max(width, height);
    for (const int e : {3840, 2560, 1920, 1280, 1024, 800, 640}) {
        const double factor = longest > 0 ? static_cast<double>(e) / longest : 1.0;
        longEdge.children.push_back(
            leaf(label(std::format("{} px", e), factor),
                 {Action::ResizeLongEdge, EditTool::Rect, static_cast<float>(e)}));
    }
    items.push_back(std::move(longEdge));
    return items;
}

void App::showResizeMenu(Point screenPos) {
    if (!current_) {
        showMessage("リサイズする画像がありません");
        return;
    }
    std::vector<EditMenuEntry> entries;
    const std::vector<MenuItem> items = buildResizeMenu(entries);
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

std::vector<MenuItem> App::buildObjectMenu(const AnnotationSpec& spec,
                                           std::vector<ObjectMenuEntry>& entries) const {
    const auto leaf = [&entries](std::string text, ObjectMenuEntry entry,
                                 bool checked = false) {
        entries.push_back(entry);
        MenuItem item;
        item.text = std::move(text);
        item.checked = checked;
        return item;
    };
    using Action = ObjectMenuEntry::Action;

    std::vector<MenuItem> items;
    if (spec.kind == AnnotationSpec::Kind::Text) {
        items.push_back(leaf("テキストを編集", {Action::EditText}));
    }
    std::string deleteText = "削除";
    if (const std::string keys = keysLabel(keymap_, Command::DeleteAnnotation); !keys.empty()) {
        deleteText += '\t';
        deleteText += keys;
    }
    items.push_back(leaf(std::move(deleteText), {Action::Delete}));
    items.push_back(menuSeparator());

    MenuItem angle;
    angle.text = std::format("回転角度 ({}°)", static_cast<int>(std::lround(spec.angleDeg)));
    for (const int a : {0, 15, 30, 45, 90, 135, 180, 270}) {
        angle.children.push_back(leaf(std::format("{}°", a),
                                      {Action::Angle, static_cast<float>(a)},
                                      static_cast<float>(a) == spec.angleDeg));
    }
    items.push_back(std::move(angle));

    // 太さ・文字サイズは画像px単位でオブジェクトへ直接適用する
    if (spec.kind == AnnotationSpec::Kind::Text) {
        MenuItem font;
        font.text =
            std::format("文字サイズ ({}px)", static_cast<int>(std::lround(spec.fontSize)));
        for (const int s : {12, 14, 18, 24, 36, 48, 72}) {
            font.children.push_back(leaf(std::format("{}px", s),
                                         {Action::FontSize, static_cast<float>(s)},
                                         static_cast<float>(s) == spec.fontSize));
        }
        items.push_back(std::move(font));

        MenuItem family;
        family.text = std::format("フォント ({})", fontFamilyLabel(spec.fontFamily));
        // 未指定(空)の注釈は既定フォントで描かれるので、そちらにチェックを付ける
        const std::string_view current = effectiveFontFamily(spec.fontFamily);
        for (const auto& [label, name] : fontFamilyChoices(current)) {
            family.children.push_back(
                leaf(label, {Action::FontFamily, 0, name}, name == current));
        }
        items.push_back(std::move(family));
    } else if (spec.kind != AnnotationSpec::Kind::Image) {
        // 貼り付けた画像は線も文字も持たないので、太さの項目は出さない
        MenuItem stroke;
        stroke.text =
            std::format("線の太さ ({}px)", static_cast<int>(std::lround(spec.strokeWidth)));
        // 手書き(特にマーカー)は太い側も要るので、選択肢を広げる
        const std::vector<int> widths = spec.kind == AnnotationSpec::Kind::Pen
                                            ? std::vector<int>{1, 2, 3, 5, 8, 12, 20, 32, 48}
                                            : std::vector<int>{1, 2, 3, 5, 8, 12, 20};
        for (const int w : widths) {
            stroke.children.push_back(leaf(std::format("{}px", w),
                                           {Action::StrokeWidth, static_cast<float>(w)},
                                           static_cast<float>(w) == spec.strokeWidth));
        }
        items.push_back(std::move(stroke));
    }
    // 線の不透明度は手書きだけ(マーカーとペンの違いはここと太さだけ)
    if (spec.kind == AnnotationSpec::Kind::Pen) {
        MenuItem alpha;
        alpha.text = std::format("線の不透明度 ({})", fillAlphaLabel(spec.strokeAlpha));
        for (const int a : {255, 178, 102, 64}) {
            alpha.children.push_back(leaf(fillAlphaLabel(a),
                                          {Action::StrokeAlpha, static_cast<float>(a)},
                                          a == spec.strokeAlpha));
        }
        items.push_back(std::move(alpha));
    }
    // 連番は後から振り直せるようにする(順序を入れ替えたくなることがある)
    if (spec.kind == AnnotationSpec::Kind::Number) {
        MenuItem number;
        number.text = std::format("番号 ({})", spec.number);
        for (int n = 1; n <= 10; ++n) {
            number.children.push_back(leaf(std::format("{}", n),
                                           {Action::Number, static_cast<float>(n)},
                                           n == spec.number));
        }
        items.push_back(std::move(number));
    }
    // 画像は自身の画素で描かれるため、色も塗りつぶしも効かない
    if (spec.kind != AnnotationSpec::Kind::Image) {
        items.push_back(
            leaf(std::format("色の変更... (#{:06X})", spec.colorRGB), {Action::PickColor}));
    }

    // 塗りつぶしは面を持つ種別だけ(直線・矢印・手書き・画像には出さない)
    if (spec.kind != AnnotationSpec::Kind::Line && spec.kind != AnnotationSpec::Kind::Arrow &&
        spec.kind != AnnotationSpec::Kind::Pen && spec.kind != AnnotationSpec::Kind::Image) {
        MenuItem fill;
        fill.text = std::format("塗りつぶし ({})", fillAlphaLabel(spec.fillAlpha));
        for (const int a : kFillAlphaChoices) {
            fill.children.push_back(leaf(fillAlphaLabel(a),
                                         {Action::FillAlpha, static_cast<float>(a)},
                                         a == spec.fillAlpha));
        }
        fill.children.push_back(menuSeparator());
        fill.children.push_back(leaf(std::format("色の変更... (#{:06X})", spec.fillRGB),
                                     {Action::PickFillColor}));
        items.push_back(std::move(fill));
    }
    if (spec.kind == AnnotationSpec::Kind::Text) {
        MenuItem border;
        border.text = std::format("枠線 ({})",
                                  borderWidthLabel(static_cast<int>(std::lround(spec.borderWidth))));
        for (const int w : kBorderWidthChoices) {
            border.children.push_back(leaf(borderWidthLabel(w),
                                           {Action::BorderWidth, static_cast<float>(w)},
                                           static_cast<float>(w) == spec.borderWidth));
        }
        border.children.push_back(menuSeparator());
        border.children.push_back(leaf(std::format("色の変更... (#{:06X})", spec.borderRGB),
                                       {Action::PickBorderColor}));
        items.push_back(std::move(border));
    }
    return items;
}

void App::showObjectMenu(Point screenPos) {
    if (!selected_ || *selected_ >= annotations_.size()) return;
    std::vector<ObjectMenuEntry> entries;
    const std::vector<MenuItem> items = buildObjectMenu(annotations_[*selected_], entries);
    const auto choice = host_.showContextMenu(items, screenPos);
    if (!choice || *choice >= entries.size()) return;
    const ObjectMenuEntry entry = entries[*choice];
    AnnotationSpec& spec = annotations_[*selected_];
    switch (entry.action) {
    case ObjectMenuEntry::Action::EditText:
        beginTextEdit(*selected_, {current_, annotations_}, false);
        textEdit_.buffer().selectAll();  // 入力し直しやすいよう全選択で始める
        notifyCaretMoved();
        host_.requestRedraw();
        return;
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
    // トグルできる属性を並べ、続いてフォント、最後に文字色。
    // 末端項目の index はこの順に対応する
    static constexpr std::array<std::pair<const char*, TextStyleFlag>, 3> kFlags{{
        {"太字", TextStyleFlag::Bold},
        {"斜体", TextStyleFlag::Italic},
        {"下線", TextStyleFlag::Underline},
    }};
    std::vector<MenuItem> items;
    for (const auto& [text, flag] : kFlags) {
        MenuItem item;
        item.text = text;
        item.checked = textEdit_.buffer().selectionHasFlag(flag);
        items.push_back(std::move(item));
    }
    items.push_back(menuSeparator());
    // 色・フォントを指定していない範囲は注釈全体のもので描かれるので、
    // そちらを見出しと初期値に使う
    const AnnotationSpec& spec = annotations_[textEdit_.index()];
    const TextStyleRun style = textEdit_.buffer().selectionStyle();
    const uint32_t initialColor = style.hasColor ? style.colorRGB : spec.colorRGB;
    // 注釈全体のフォントはメニュー表示後にも使うため、参照ではなく値で持つ
    const std::string wholeFamily{effectiveFontFamily(spec.fontFamily)};
    const std::string_view currentFamily = style.fontFamily.empty()
                                               ? std::string_view(wholeFamily)
                                               : std::string_view(style.fontFamily);

    // 末端 index: 0-2 が太字・斜体・下線、続いてフォントの候補、最後に文字色
    const std::vector<std::pair<std::string, std::string>> families =
        fontFamilyChoices(currentFamily);
    MenuItem family;
    family.text = std::format("フォント ({})", fontFamilyLabel(currentFamily));
    for (const auto& [label, name] : families) {
        MenuItem item;
        item.text = label;
        item.checked = name == currentFamily;
        family.children.push_back(std::move(item));
    }
    items.push_back(std::move(family));

    MenuItem color;
    color.text = std::format("文字色... (#{:06X})", initialColor);
    items.push_back(std::move(color));

    const size_t familyBase = kFlags.size();                 // フォント候補の先頭
    const size_t colorIndex = familyBase + families.size();  // 文字色
    const auto choice = host_.showContextMenu(items, screenPos);
    bool changed = false;
    if (choice && *choice < kFlags.size()) {
        changed = textEdit_.buffer().toggleSelectionFlag(kFlags[*choice].second);
    } else if (choice && *choice < colorIndex) {
        // 注釈全体と同じフォントを選んだら指定を外す(範囲を残さず、
        // 以降は全体のフォント変更に追従する)
        const std::string& picked = families[*choice - familyBase].second;
        changed = textEdit_.buffer().setSelectionFontFamily(
            picked == wholeFamily ? std::string() : picked);
    } else if (choice && *choice == colorIndex) {
        if (const auto rgb = host_.showColorPicker(initialColor)) {
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
        editStrokeWidth_ = entry.value;
        return false;
    case EditMenuEntry::Action::FontSize:
        editFontSize_ = entry.value;
        return false;
    case EditMenuEntry::Action::FontFamily:
        editFontFamily_ = entry.family;
        return false;
    case EditMenuEntry::Action::PickColor:
        if (const auto rgb = host_.showColorPicker(editColorRGB_)) editColorRGB_ = *rgb;
        return false;
    case EditMenuEntry::Action::FillAlpha:
        editFillAlpha_ = static_cast<int>(entry.value);
        return false;
    case EditMenuEntry::Action::PickFillColor:
        if (const auto rgb = host_.showColorPicker(editFillRGB_)) {
            editFillRGB_ = *rgb;
            // 色を選んだのに塗られないままにならないよう、塗りなしなら不透明で始める
            if (editFillAlpha_ == 0) editFillAlpha_ = 255;
        }
        return false;
    case EditMenuEntry::Action::BorderWidth:
        editBorderWidth_ = entry.value;
        return false;
    case EditMenuEntry::Action::PickBorderColor:
        if (const auto rgb = host_.showColorPicker(editBorderRGB_)) {
            editBorderRGB_ = *rgb;
            if (editBorderWidth_ <= 0) editBorderWidth_ = 1.0f;
        }
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

bool App::applyCrop() {
    if (!current_) return false;
    // 部分的にかかったピクセルも含める(floor/ceil)
    const Point p1 = drag_.startImage();
    const Point p2 = drag_.endImage();
    const int x0 = static_cast<int>(std::floor(std::min(p1.x, p2.x)));
    const int y0 = static_cast<int>(std::floor(std::min(p1.y, p2.y)));
    const int x1 = static_cast<int>(std::ceil(std::max(p1.x, p2.x)));
    const int y1 = static_cast<int>(std::ceil(std::max(p1.y, p2.y)));
    auto cropped = cropImage(*current_, {x0, y0, x1 - x0, y1 - y0});
    if (!cropped) return false;
    pushUndo();
    current_ = std::move(cropped);
    // 注釈はオブジェクトのまま維持し、切り出した原点ぶんだけ平行移動する
    for (AnnotationSpec& spec : annotations_) {
        translateAnnotation(spec, static_cast<float>(-x0), static_cast<float>(-y0));
    }
    viewport_.setImage(
        {static_cast<float>(current_->width), static_cast<float>(current_->height)});
    edited_ = true;
    updateTitle();
    return true;
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
    const Point p1 = drag_.startImage();
    const Point p2 = drag_.endImage();
    const int x0 = static_cast<int>(std::floor(std::min(p1.x, p2.x)));
    const int y0 = static_cast<int>(std::floor(std::min(p1.y, p2.y)));
    const int x1 = static_cast<int>(std::ceil(std::max(p1.x, p2.x)));
    const int y1 = static_cast<int>(std::ceil(std::max(p1.y, p2.y)));
    auto region = cropImage(*current_, {x0, y0, x1 - x0, y1 - y0});
    if (!region) {
        showMessage("選択した範囲が画像の外です");
        return false;
    }
    requestOcr(region);
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
    spec.colorRGB = editColorRGB_;
    spec.fillRGB = editFillRGB_;
    spec.fillAlpha = editFillAlpha_;
    spec.borderRGB = editBorderRGB_;
    spec.fontFamily = editFontFamily_;
    // 線幅・文字サイズ・枠線幅は画像座標そのまま。表示倍率では変えない
    // (同じ設定で描いたものが、描いたときのズームによらず同じ太さになるように)
    spec.strokeWidth = editStrokeWidth_;
    spec.fontSize = editFontSize_;
    spec.borderWidth = editBorderWidth_;
    if (kind == AnnotationSpec::Kind::Pen) {
        // 軌跡そのものが図形。p1/p2 は選択領域ではなく点列の bbox に合わせる
        spec.points = drag_.penPoints();
        updatePenBounds(spec);
        if (tool_ == EditTool::Marker) {
            spec.strokeWidth = editStrokeWidth_ * kMarkerWidthScale;
            spec.strokeAlpha = kMarkerAlpha;
        }
    } else if (kind == AnnotationSpec::Kind::Number) {
        // 円の中の数字は自動で色が決まるため、色設定は円の塗りとして使う
        spec.number = nextMarkerNumber();
        spec.fillRGB = editColorRGB_;
        spec.fillAlpha = 255;
        // 小さすぎる円は数字が潰れるだけなので、始点側を固定して最小の大きさまで広げる
        const float minSize = std::max(8.0f, editFontSize_ * 1.8f);
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
    // トリミングは切り出す範囲、テキストは中身の無い箱、文字認識は読み取る範囲で、
    // どれも実物を描けない。その 3 つはラバーバンド(App::selection)に任せる
    return drag_.dragging() && current_ != nullptr && tool_ != EditTool::Crop &&
           tool_ != EditTool::Text && tool_ != EditTool::Ocr;
}

void App::updatePreview() {
    if (!previewVisible()) return;
    previewSpec_ = makeAnnotationSpec(kindOfTool(tool_));
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
        EditSnapshot before{current_, annotations_};  // 追加前の状態を undo 用に控える
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

bool App::executePasteImage() {
    auto image = clipboard_.getImage();
    if (!image || image->width == 0 || image->height == 0) {
        showMessage("クリップボードに画像がありません");
        return false;
    }
    discardEdits();
    resetSequence();  // 貼り付け画像は 1 枚きり(再生中なら止まる)
    current_ = std::move(image);
    clipboardImage_ = true;
    displayedPath_.clear();  // 一覧に戻ったとき必ず再取得させる
    loadFailed_ = false;
    loadError_.clear();
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
    if (clipboardImage_ || list_.empty()) {
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
    const std::string defaultName = clipboardImage_ || list_.empty()
                                        ? "クリップボード.png"
                                        : pathToUtf8(list_.current().stem()) + ".png";
    if (const auto path = host_.showSaveDialog(defaultName)) {
        // 一覧の現在のファイルを選び直した場合も上書きなので同じ後始末をする
        const bool isOverwrite =
            !clipboardImage_ && !list_.empty() && samePath(*path, list_.current());
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
    const std::string jobName =
        clipboardImage_ || list_.empty() ? "クリップボードの画像"
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
    if (edited_) {
        edited_ = false;
        updateTitle();
    }
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
    if (edited_) return;
    edited_ = true;
    updateTitle();
}

void App::pushUndo() {
    pushUndoState({current_, annotations_});
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
    if (!restoreFrom(history_.undo({current_, annotations_}))) {
        showMessage("取り消す編集はありません");
        return;
    }
    // 履歴を使い切った = 開いた直後の状態に戻った
    edited_ = history_.canUndo();
    updateTitle();
    host_.requestRedraw();
}

void App::executeRedo() {
    if (textEdit_.active()) commitTextEdit();
    if (!restoreFrom(history_.redo({current_, annotations_}))) {
        showMessage("やり直す編集はありません");
        return;
    }
    edited_ = true;  // やり直した先は必ず何らかの編集が入った状態
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
    selected_.reset();  // index が指す対象が変わりうるため選択は解除する
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
    if (history_.empty() && !edited_) return;
    history_.clear();
    if (edited_) {
        edited_ = false;
        showMessage("編集を破棄しました");
    }
}

Point App::clampToImage(Point imagePos) const {
    if (!current_) return imagePos;
    return {std::clamp(imagePos.x, 0.0f, static_cast<float>(current_->width)),
            std::clamp(imagePos.y, 0.0f, static_cast<float>(current_->height))};
}

Point App::dragEndImage(Point screenPos, bool shift) const {
    const Point p = clampToImage(imageToScreen().inverted().apply(screenPos));
    // 連番マーカーは常に円にしたいので、Shift の有無によらず正方形へ寄せる
    if (tool_ == EditTool::Number) return constrainToSquare(drag_.startImage(), p);
    if (!shift) return p;
    // 手書きは軌跡そのものが図形なので、選択領域の形ではなく線の向きを揃える。
    // 起点は Shift を押した時点の点(それまでに描いた軌跡はそのまま残る)
    if (penToolActive()) {
        const auto anchor = drag_.straightAnchorPoint();
        return anchor ? constrainToAxis(*anchor, p) : p;
    }
    // 直線・矢印は bbox ではなく線の向きを揃えたいので、正方形化(=45 度固定)
    // ではなく水平・垂直・45 度へのスナップにする
    if (tool_ == EditTool::Line || tool_ == EditTool::Arrow) {
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
    // ドラッグ中とテキスト編集中は出さない(操作の途中で押せてしまうと編集が消える)
    if (pointer_.panning() || drag_.dragging() || sidebar_.resizing() || textEdit_.active() ||
        textEdit_.mouseSelecting() || objectDrag_.active()) {
        return {};
    }
    const float offset = sidebarOffset();
    const float barHeight = statusBarVisible() ? kStatusBarHeight : 0.0f;
    const SizeF viewport{clientSize_.w - offset, clientSize_.h - barHeight};
    // 貼り付け画像の表示中は、前後どちらでもフォルダ一覧の表示へ戻れる
    const bool hasPrev = clipboardImage_ || list_.index() > 0;
    const bool hasNext = clipboardImage_ || list_.index() + 1 < list_.size();
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
    if (clipboardImage_) return;  // 貼り付け画像の表示はデコード完了で上書きしない
    if (edited_) return;          // 編集中の画像も同様
    if (list_.empty()) return;
    // 表示すべき画像がまだ画面に出ていなければ取得を再試行する
    if (displayedPath_ == list_.current() && (current_ || loadFailed_)) {
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
    if (!current_ || displayedPath_.empty()) return;
    // 多フレームの画像では tryGet が返すのは先頭フレーム。別のフレームを表示している
    // ときに拾うと絵が飛ぶので、フレーム列は adoptSequence に任せる
    if (multiFrame()) return;
    auto latest = cache_.tryGet(displayedPath_);
    if (!latest || latest == current_) return;
    // 大きさが変わる差し替えは来ない想定だが、来たら座標系が食い違うので拒む
    if (latest->width != current_->width || latest->height != current_->height) return;
    current_ = std::move(latest);
    host_.requestRedraw();  // ズーム・パン・注釈はそのまま(画素だけ入れ替わる)
}

void App::adoptSequence() {
    if (displayedPath_.empty() || clipboardImage_ || edited_) return;
    auto latest = cache_.tryGetSequence(displayedPath_);
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
        cache_.requestFrame(displayedPath_, static_cast<uint32_t>(index));
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
    if (edited_) {
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
    if (moved || clipboardImage_) refreshCurrent();
}

void App::refreshCurrent() {
    discardEdits();
    resetSequence();          // 前の画像のフレーム列と再生状態を捨てる
    clipboardImage_ = false;  // 表示をフォルダ一覧由来に戻す
    if (list_.empty()) {
        current_.reset();
        displayedPath_.clear();
        loadFailed_ = false;
        loadError_.clear();
        updateTitle();
        host_.requestRedraw();
        return;
    }
    const fs::path& path = list_.current();
    bool failed = false;
    std::string error;
    if (auto image = cache_.tryGet(path, &failed, &error)) {
        current_ = std::move(image);
        displayedPath_ = path;
        loadFailed_ = false;
        loadError_.clear();
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
        displayedPath_ = path;
        loadFailed_ = true;
        loadError_ = std::move(error);
    } else {
        // デコード待ち。前の画像を表示したまま onDecodeCompleted を待つ
        cache_.requestNow(path);
        loadFailed_ = false;
        loadError_.clear();
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
    const int x = static_cast<int>(std::floor(p.x));
    const int y = static_cast<int>(std::floor(p.y));
    if (x < 0 || y < 0 || x >= static_cast<int>(current_->width) ||
        y >= static_cast<int>(current_->height)) {
        return {};
    }
    const uint8_t* px =
        current_->pixels.data() + (static_cast<size_t>(y) * current_->width + x) * 4;
    const uint8_t a = px[3];
    const uint8_t r = unpremultiply(px[2], a);
    const uint8_t g = unpremultiply(px[1], a);
    const uint8_t b = unpremultiply(px[0], a);
    std::string text = std::format("({}, {})  #{:02X}{:02X}{:02X}", x, y, r, g, b);
    text += a == 255 ? std::format("  RGB({}, {}, {})", r, g, b)
                     : std::format("  RGBA({}, {}, {}, {})", r, g, b, a);
    return text;
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
    if (!message_.empty()) {
        bar.leftText = message_;
    } else if (current_) {
        // 編集ドラッグが何をするかは見た目に出ないので、現在のツールをここに出す。
        // 縮小して取り込んだ画像は、ズーム率が元の大きさに対する比でなくなるので明示する
        std::string text =
            current_->downscaled()
                ? std::format("{} x {} px (元 {} x {} を縮小表示)", current_->width,
                              current_->height, current_->sourceWidth, current_->sourceHeight)
                : std::format("{} x {} px", current_->width, current_->height);
        // 何枚目を見ているのかはタイトルの [i/n](フォルダ内の位置)では分からない
        if (multiFrame()) {
            text += std::format("  |  {} {}/{}", frameUnitLabel(), playback_.index + 1,
                                sequence_->frames.size());
            if (const std::string& label = sequence_->frames[playback_.index].label;
                !label.empty()) {
                text += std::format(" ({})", label);
            }
            if (sequence_->kind == SequenceKind::Animation) {
                text += playback_.playing ? " 再生中" : " 停止中";
            }
        }
        // なぜ隣のフォルダの画像が出てくるのか分からなくなるので、再帰中は常に見せる
        if (recursive_) text += "  |  サブフォルダ含む";
        text += std::format("  |  ツール: {}", toolLabel(tool_));
        bar.leftText = std::move(text);
    } else if (loadFailed_) {
        // 失敗した段階とコードまで出す(現物が手元にない不具合を切り分けられるように)
        bar.leftText = loadError_.empty() ? "読み込み失敗"
                                          : std::format("読み込み失敗: {}", loadError_);
    }
    bar.rightText = hoverText_;
    return bar;
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
    if (clipboardImage_) {
        host_.setTitle(std::format("(クリップボード){} {}% - {}",
                                   edited_ ? " (編集済み)" : "",
                                   static_cast<int>(std::lround(viewport_.zoom() * 100)), appName));
        return;
    }
    if (list_.empty()) {
        host_.setTitle(appName);
        return;
    }
    std::string title = std::format("{} [{}/{}]", pathToUtf8(list_.current().filename()),
                                    list_.index() + 1, list_.size());
    if (loadFailed_) {
        title += " (読み込み失敗)";
    } else if (displayedPath_ != list_.current()) {
        title += " (読み込み中)";
    } else {
        if (edited_) title += " (編集済み)";
        title += std::format(" {}%", static_cast<int>(std::lround(viewport_.zoom() * 100)));
    }
    title += " - " + appName;
    host_.setTitle(title);
}

} // namespace blinker
