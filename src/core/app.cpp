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
#include "core/help.h"
#include "core/pixel_convert.h"
#include "core/str_util.h"
#include "core/unicode.h"
#include "core/version.h"

namespace blinker {

namespace fs = std::filesystem;

namespace {

constexpr unsigned kMessageDurationMs = 3000;

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
constexpr std::array<std::pair<const char*, const char*>, 8> kFontFamilyChoices{{
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
    case EditTool::Text:    return "テキスト";
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
    if (lower == "text") return EditTool::Text;
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
    case EditTool::Text:    return Command::SelectToolText;
    }
    return Command::None;
}

// 図形ツールが作る注釈の種別。Crop は注釈ではないので Rect を返す(呼ばれない)
AnnotationSpec::Kind kindOfTool(EditTool tool) {
    switch (tool) {
    case EditTool::Ellipse: return AnnotationSpec::Kind::Ellipse;
    case EditTool::Arrow:   return AnnotationSpec::Kind::Arrow;
    case EditTool::Line:    return AnnotationSpec::Kind::Line;
    case EditTool::Text:    return AnnotationSpec::Kind::Text;
    case EditTool::Crop:
    case EditTool::Rect:    break;
    }
    return AnnotationSpec::Kind::Rect;
}

} // namespace

App::App(IAppHost& host, IFileSystem& fileSystem, ImageCache& cache, IClipboard& clipboard,
         IImageEncoder& encoder, IAnnotationRasterizer& rasterizer)
    : host_(host),
      fileSystem_(fileSystem),
      cache_(cache),
      clipboard_(clipboard),
      encoder_(encoder),
      rasterizer_(rasterizer) {}

void App::applyConfig(const Config& config) {
    keymap_.applyConfig(config.section("keys"));
    backgroundRGB_ = config.getColorRGB("view", "background", backgroundRGB_);
    viewport_.setFitUpscale(config.getBool("view", "fit_upscale", false));
    prefetchRadius_ = std::clamp(config.getInt("view", "prefetch_radius", prefetchRadius_), 0, 8);
    statusBarEnabled_ = config.getBool("view", "statusbar", statusBarEnabled_);
    sidebarEnabled_ = config.getBool("view", "sidebar", sidebarEnabled_);
    sidebarWidth_ = static_cast<float>(
        std::clamp(config.getInt("view", "sidebar_width", static_cast<int>(sidebarWidth_)), 120,
                   480));
    helpHintEnabled_ = config.getBool("view", "help_hint", helpHintEnabled_);
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
    // 起動時の(右ドラッグで実行される)ツール。未指定・未知の名前なら既定のまま
    if (const auto tool = toolFromName(config.get("edit", "tool"))) setTool(*tool);
    applyLayout();
}

bool App::showHelpHint() {
    if (!helpHintEnabled_) return false;
    // 既に一覧が出ているなら案内は不要
    if (sidebarEnabled_ && sidebarMode_ == SidebarMode::Help) return false;
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
    list_.set(fileSystem_.listImages(dir), isDirectory ? fs::path{} : path);
    current_.reset();
    displayedPath_.clear();
    loadFailed_ = false;
    loadError_.clear();
    refreshCurrent();
}

void App::execute(Command command) {
    switch (command) {
    // 移動系は貼り付け画像の表示中ならフォルダ一覧の表示へ戻る(一覧位置が
    // 動かなくても refreshCurrent が必要なため clipboardImage_ を OR する)
    case Command::NextImage:
        if (list_.next() || clipboardImage_) refreshCurrent();
        break;
    case Command::PrevImage:
        if (list_.prev() || clipboardImage_) refreshCurrent();
        break;
    case Command::FirstImage:
        if (list_.first() || clipboardImage_) refreshCurrent();
        break;
    case Command::LastImage:
        if (list_.last() || clipboardImage_) refreshCurrent();
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
        onDragPan(kPanStepPx, 0);
        break;
    case Command::PanRight:
        onDragPan(-kPanStepPx, 0);
        break;
    case Command::PanUp:
        onDragPan(0, kPanStepPx);
        break;
    case Command::PanDown:
        onDragPan(0, -kPanStepPx);
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
        if (!current_) {
            showMessage("コピーする画像がありません");
        } else if (clipboard_.setImage(*compositeImage())) {
            showMessage("画像をクリップボードにコピーしました");
        } else {
            showMessage("画像のコピーに失敗しました");
        }
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
    case Command::PasteImage:
        if (auto image = clipboard_.getImage(); image && image->width > 0 && image->height > 0) {
            discardEdits();
            current_ = std::move(image);
            clipboardImage_ = true;
            displayedPath_.clear();  // 一覧に戻ったとき必ず再取得させる
            loadFailed_ = false;
            loadError_.clear();
            viewport_.setImage(
                {static_cast<float>(current_->width), static_cast<float>(current_->height)});
            updateTitle();
            host_.requestRedraw();
        } else {
            showMessage("クリップボードに画像がありません");
        }
        break;
    case Command::SaveImageAs: {
        if (!current_) {
            showMessage("保存する画像がありません");
            break;
        }
        const std::string defaultName = clipboardImage_ || list_.empty()
                                            ? "クリップボード.png"
                                            : pathToUtf8(list_.current().stem()) + ".png";
        if (const auto path = host_.showSaveDialog(defaultName)) {
            if (encoder_.encode(*compositeImage(), *path)) {
                showMessage("保存しました: " + pathToUtf8(*path));
            } else {
                showMessage("保存に失敗しました: " + pathToUtf8(*path));
            }
        }
        break;
    }
    case Command::Undo:
        executeUndo();
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
    case Command::SelectToolText:
        setTool(EditTool::Text);
        break;
    case Command::ToggleSidebar:
        // 操作一覧が出ている間は、閉じるのではなくファイル名一覧へ切り替える
        sidebarEnabled_ = !(sidebarEnabled_ && sidebarMode_ == SidebarMode::Files);
        sidebarMode_ = SidebarMode::Files;
        applyLayout();
        if (sidebarEnabled_) scrollSidebarToCurrent();
        onViewChanged();  // フィット再計算でズーム率表示が変わりうる
        break;
    case Command::ToggleHelp:
        sidebarEnabled_ = !(sidebarEnabled_ && sidebarMode_ == SidebarMode::Help);
        if (sidebarEnabled_) {
            sidebarMode_ = SidebarMode::Help;
            // ini 適用後のキーバインドから作る。開くたびに作り直すので設定変更にも追従する
            helpLines_ = buildHelpLines(keymap_);
            sidebarScroll_ = 0;
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
            objectDrag_ = ObjectDrag::None;
            host_.requestRedraw();
        } else if (selecting_) {
            selecting_ = false;
            host_.requestRedraw();
        } else if (sidebarEnabled_ && sidebarMode_ == SidebarMode::Help) {
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
    if (textEditing_) return handleTextEditKey(chord);
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

void App::onWheel(float wheelNotches, Point screenPos) {
    if (wheelNotches == 0) return;
    if (sidebarVisible() && screenPos.x < sidebarOffset()) {
        // サイドバー上ではズームせず一覧をスクロール(1ノッチ = 3項目)
        sidebarScroll_ -= wheelNotches * 3 * kSidebarItemHeight;
        clampSidebarScroll();
        host_.requestRedraw();
        return;
    }
    viewport_.zoomAt(std::pow(Viewport::kZoomStep, wheelNotches),
                     {screenPos.x - sidebarOffset(), screenPos.y});
    onViewChanged();
}

bool App::onMouseDown(Point screenPos) {
    if (sidebarVisible() && screenPos.x < sidebarOffset()) {
        // ステータスバー上(サイドバー領域外)はどちらの操作でもないため消費だけする
        if (textEditing_) commitTextEdit();  // 画像切替の前に編集を確定する
        if (sidebarMode_ == SidebarMode::Files && screenPos.y >= 0 &&
            screenPos.y < sidebarViewHeight()) {
            const size_t index =
                static_cast<size_t>((screenPos.y + sidebarScroll_) / kSidebarItemHeight);
            if (index < list_.size() && (list_.jumpTo(index) || clipboardImage_)) {
                refreshCurrent();
            }
        }
        return true;  // サイドバー上のクリックはパン開始にしない
    }
    if (!current_) return false;
    const float barHeight = statusBarVisible() ? kStatusBarHeight : 0.0f;
    if (screenPos.y >= clientSize_.h - barHeight) return false;
    objectDrag_ = ObjectDrag::None;
    dragUndoPushed_ = false;
    // 選択中オブジェクトのハンドル(スクリーン座標で判定)→ 回転 / リサイズ開始
    if (selected_ && *selected_ < annotations_.size()) {
        const AnnotationSpec& spec = annotations_[*selected_];
        const Point handle = rotationHandlePos(spec, imageToScreen(), kRotationHandleOffsetPx);
        const float dx = screenPos.x - handle.x;
        const float dy = screenPos.y - handle.y;
        if (dx * dx + dy * dy <= kRotationHandleHitPx * kRotationHandleHitPx) {
            objectDrag_ = ObjectDrag::Rotate;
            dragOrigSpec_ = spec;
            dragStartAngleDeg_ =
                angleDegFrom(imageToScreen().apply(annotationCenter(spec)), screenPos);
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
            objectDrag_ = ObjectDrag::Resize;
            dragResizeHandle_ = nearest->handle;
            dragOrigSpec_ = spec;
            return true;
        }
    }
    const Point imagePos = imageToScreen().inverted().apply(screenPos);
    const float tolerance = kHitTolerancePx / std::max(viewport_.zoom(), 0.001f);
    // 編集中: 枠内のクリックはキャレット移動と範囲選択の開始。枠外なら確定して通常処理へ
    if (textEditing_ && textEditIndex_ < annotations_.size()) {
        if (hitTestAnnotation(annotations_[textEditIndex_], imagePos, tolerance)) {
            if (isComposing()) return true;  // 変換中は位置が動かないようにする
            textBuffer_.setCaret(textOffsetAt(imagePos), false);
            textEditMouseSelect_ = true;
            notifyCaretMoved();
            host_.requestRedraw();
            return true;
        }
        commitTextEdit();
    }
    // 注釈本体 → 選択して移動ドラッグ開始。外れたら選択解除してパンに回す
    if (const auto hit = hitTestAnnotations(annotations_, imagePos, tolerance)) {
        selected_ = hit;
        objectDrag_ = ObjectDrag::Move;
        dragStartImage_ = imagePos;
        dragOrigSpec_ = annotations_[*hit];
        host_.requestRedraw();
        return true;
    }
    if (selected_) {
        selected_.reset();
        host_.requestRedraw();
    }
    return false;
}

void App::onMouseUp() {
    textEditMouseSelect_ = false;
    // テキストの高さは内容で決まるため、リサイズ確定時に折り返し後の実寸へ揃える。
    // 編集中は利用者が決めた枠幅を保ちたいので高さだけ合わせる
    if (objectDrag_ == ObjectDrag::Resize && dragUndoPushed_ && selected_ &&
        *selected_ < annotations_.size() &&
        annotations_[*selected_].kind == AnnotationSpec::Kind::Text) {
        const bool changed = textEditing_ ? measureTextHeight(annotations_[*selected_])
                                          : measureTextExtent(annotations_[*selected_]);
        if (changed) {
            if (textEditing_) notifyCaretMoved();
            host_.requestRedraw();
        }
    }
    objectDrag_ = ObjectDrag::None;
    dragUndoPushed_ = false;
}

bool App::onDoubleClick(Point screenPos) {
    if (!current_) return false;
    if (sidebarVisible() && screenPos.x < sidebarOffset()) return false;
    const Point imagePos = imageToScreen().inverted().apply(screenPos);
    const float tolerance = kHitTolerancePx / std::max(viewport_.zoom(), 0.001f);
    // 編集中の枠内でのダブルクリックは語の選択
    if (textEditing_ && textEditIndex_ < annotations_.size() &&
        hitTestAnnotation(annotations_[textEditIndex_], imagePos, tolerance)) {
        if (isComposing()) return true;  // 変換中は選択を変えない
        textBuffer_.selectWordAt(textOffsetAt(imagePos));
        notifyCaretMoved();
        host_.requestRedraw();
        return true;
    }
    const auto hit = hitTestAnnotations(annotations_, imagePos, tolerance);
    if (!hit || annotations_[*hit].kind != AnnotationSpec::Kind::Text) return false;
    if (textEditing_) commitTextEdit();
    selected_ = hit;
    objectDrag_ = ObjectDrag::None;
    beginTextEdit(*hit, {current_, annotations_}, false);
    // 文字位置はダブルクリックした場所の語を選ぶ(通常のテキスト編集と同じ)
    textBuffer_.selectWordAt(textOffsetAt(imagePos));
    notifyCaretMoved();
    host_.requestRedraw();
    return true;
}

void App::onRightDragStart(Point screenPos) {
    if (!current_) return;
    // 編集中の選択範囲の上での右クリックは、確定せずに書式メニューを出す
    if (textEditing_ && textEditIndex_ < annotations_.size() && !isComposing() &&
        textBuffer_.hasSelection()) {
        const Point imagePos = imageToScreen().inverted().apply(screenPos);
        const float tolerance = kHitTolerancePx / std::max(viewport_.zoom(), 0.001f);
        if (hitTestAnnotation(annotations_[textEditIndex_], imagePos, tolerance)) {
            textStyleMenuPending_ = true;  // メニューはボタンを離した位置で出す
            return;
        }
    }
    if (textEditing_) commitTextEdit();  // 右操作は編集の外側なので先に確定する
    // サイドバー・ステータスバー上からは開始しない
    const float barHeight = statusBarVisible() ? kStatusBarHeight : 0.0f;
    if (screenPos.x < sidebarOffset() || screenPos.y >= clientSize_.h - barHeight) return;
    selecting_ = true;
    selStartScreen_ = screenPos;
    selStartImage_ = clampToImage(imageToScreen().inverted().apply(screenPos));
    selEndImage_ = selStartImage_;
    updatePreview();
    host_.requestRedraw();
}

void App::onRightDragMove(Point screenPos, bool shift) {
    if (!selecting_) return;
    selEndImage_ = dragEndImage(screenPos, shift);
    updatePreview();
    host_.requestRedraw();
}

void App::onRightDragEnd(Point screenPos, bool shift) {
    if (textStyleMenuPending_) {
        textStyleMenuPending_ = false;
        showTextStyleMenu(screenPos);
        return;
    }
    if (!selecting_) return;
    selecting_ = false;
    selEndImage_ = dragEndImage(screenPos, shift);
    const float dx = screenPos.x - selStartScreen_.x;
    const float dy = screenPos.y - selStartScreen_.y;
    if (dx * dx + dy * dy < kDragThresholdPx * kDragThresholdPx) {
        // 単なる右クリック(移動量が小さい): 注釈の上ならオブジェクトメニュー、
        // そうでなければツール切り替えメニューを出す
        const Point imagePos = imageToScreen().inverted().apply(screenPos);
        const float tolerance = kHitTolerancePx / std::max(viewport_.zoom(), 0.001f);
        if (const auto hit = hitTestAnnotations(annotations_, imagePos, tolerance)) {
            selected_ = hit;
            host_.requestRedraw();
            showObjectMenu(screenPos);
        } else {
            host_.requestRedraw();
            showToolMenu(screenPos);
        }
        return;
    }
    // 事前に選んであるツールをそのまま適用する(メニューは出さない)。
    // ドラッグ中のプレビューはここで実物の注釈へ置き換わる
    applyCurrentTool();
    host_.requestRedraw();
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
    applyAnnotation(kindOfTool(tool_));
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
    // ツールは選ぶだけで、実際の適用は次の右ドラッグで行う。現在のツールにチェックが付く。
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
    items.push_back(menuSeparator());
    items.push_back(tool(EditTool::Rect));
    items.push_back(tool(EditTool::Ellipse));
    items.push_back(tool(EditTool::Arrow));
    items.push_back(tool(EditTool::Line));
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
    return items;
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
    } else {
        MenuItem stroke;
        stroke.text =
            std::format("線の太さ ({}px)", static_cast<int>(std::lround(spec.strokeWidth)));
        for (const int w : {1, 2, 3, 5, 8, 12, 20}) {
            stroke.children.push_back(leaf(std::format("{}px", w),
                                           {Action::StrokeWidth, static_cast<float>(w)},
                                           static_cast<float>(w) == spec.strokeWidth));
        }
        items.push_back(std::move(stroke));
    }
    items.push_back(
        leaf(std::format("色の変更... (#{:06X})", spec.colorRGB), {Action::PickColor}));

    // 塗りつぶしは面を持つ種別だけ(直線・矢印には出さない)
    if (spec.kind != AnnotationSpec::Kind::Line && spec.kind != AnnotationSpec::Kind::Arrow) {
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
        textBuffer_.selectAll();  // 入力し直しやすいよう全選択で始める
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
    if (!textEditing_ || !textBuffer_.hasSelection()) return;
    if (textEditIndex_ >= annotations_.size()) return;
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
        item.checked = textBuffer_.selectionHasFlag(flag);
        items.push_back(std::move(item));
    }
    items.push_back(menuSeparator());
    // 色・フォントを指定していない範囲は注釈全体のもので描かれるので、
    // そちらを見出しと初期値に使う
    const AnnotationSpec& spec = annotations_[textEditIndex_];
    const TextStyleRun style = textBuffer_.selectionStyle();
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
        changed = textBuffer_.toggleSelectionFlag(kFlags[*choice].second);
    } else if (choice && *choice < colorIndex) {
        // 注釈全体と同じフォントを選んだら指定を外す(範囲を残さず、
        // 以降は全体のフォント変更に追従する)
        const std::string& picked = families[*choice - familyBase].second;
        changed = textBuffer_.setSelectionFontFamily(picked == wholeFamily ? std::string()
                                                                          : picked);
    } else if (choice && *choice == colorIndex) {
        if (const auto rgb = host_.showColorPicker(initialColor)) {
            changed = textBuffer_.setSelectionColor(*rgb);
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
    }
    return true;
}

bool App::applyCrop() {
    if (!current_) return false;
    // 部分的にかかったピクセルも含める(floor/ceil)
    const int x0 = static_cast<int>(std::floor(std::min(selStartImage_.x, selEndImage_.x)));
    const int y0 = static_cast<int>(std::floor(std::min(selStartImage_.y, selEndImage_.y)));
    const int x1 = static_cast<int>(std::ceil(std::max(selStartImage_.x, selEndImage_.x)));
    const int y1 = static_cast<int>(std::ceil(std::max(selStartImage_.y, selEndImage_.y)));
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

AnnotationSpec App::makeAnnotationSpec(AnnotationSpec::Kind kind) const {
    AnnotationSpec spec;
    spec.kind = kind;
    spec.p1 = selStartImage_;
    spec.p2 = selEndImage_;
    spec.colorRGB = editColorRGB_;
    spec.fillRGB = editFillRGB_;
    spec.fillAlpha = editFillAlpha_;
    spec.borderRGB = editBorderRGB_;
    spec.fontFamily = editFontFamily_;
    // 線幅・文字サイズ・枠線幅は「画面上での見た目」基準で画像座標へ換算する
    const float zoom = std::max(viewport_.zoom(), 0.001f);
    spec.strokeWidth = std::max(1.0f, editStrokeWidth_ / zoom);
    spec.fontSize = std::max(4.0f, editFontSize_ / zoom);
    spec.borderWidth = editBorderWidth_ > 0 ? std::max(1.0f, editBorderWidth_ / zoom) : 0.0f;
    return spec;
}

bool App::previewVisible() const {
    // トリミングは切り出す範囲、テキストは中身の無い箱で、どちらも実物を描けない。
    // その 2 つはラバーバンド(App::selection)に任せる
    return selecting_ && current_ != nullptr && tool_ != EditTool::Crop &&
           tool_ != EditTool::Text;
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
        UndoState before{current_, annotations_};  // 追加前の状態を undo 用に控える
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

void App::beginTextEdit(size_t index, UndoState before, bool created) {
    if (index >= annotations_.size()) return;
    textEditing_ = true;
    textEditIndex_ = index;
    textEditCreated_ = created;
    textEditMouseSelect_ = false;
    textEditCaretOn_ = true;
    textUndoPushed_ = false;
    textUndoState_ = std::move(before);
    resetComposition();
    textStyleMenuPending_ = false;
    // キャレットは末尾。部分書式も引き継いで、続きの入力が直前の書式を継ぐようにする
    textBuffer_ = TextEditBuffer(annotations_[index].text, annotations_[index].styles);
    selected_ = index;
    objectDrag_ = ObjectDrag::None;
    notifyCaretMoved();
}

void App::commitTextEdit() {
    if (!textEditing_) return;
    textEditing_ = false;
    textEditMouseSelect_ = false;
    resetComposition();  // 変換中なら捨てる(host 側も IME へキャンセルを通知する)
    host_.setTextEditing(false, {}, 0);
    if (textEditIndex_ < annotations_.size()) {
        AnnotationSpec& spec = annotations_[textEditIndex_];
        spec.text = textBuffer_.text();  // 変換中文字列を落とした確定内容にする
        spec.styles = textBuffer_.styles();
        if (spec.text.empty()) {
            // 空のテキストボックスは残さない(新規・既存とも削除する)
            pushTextEditUndoOnce();
            annotations_.erase(annotations_.begin() +
                               static_cast<std::ptrdiff_t>(textEditIndex_));
            selected_.reset();
            if (!textEditCreated_) markEdited();
        } else if (!measureTextExtent(spec)) {
            showMessage("描画に失敗しました");
        }
    }
    host_.requestRedraw();
}

void App::cancelTextEdit() {
    if (!textEditing_) return;
    textEditing_ = false;
    textEditMouseSelect_ = false;
    resetComposition();
    host_.setTextEditing(false, {}, 0);
    // 変更を記録済みなら undo と同じ経路で編集前へ戻す。新規作成中なら追加ごと消える
    if (textUndoPushed_) {
        executeUndo();
        return;
    }
    if (textEditCreated_ && textEditIndex_ < annotations_.size()) {
        annotations_.erase(annotations_.begin() + static_cast<std::ptrdiff_t>(textEditIndex_));
        selected_.reset();
    }
    host_.requestRedraw();
}

void App::pushTextEditUndoOnce() {
    if (textUndoPushed_) return;
    pushUndoState(std::move(textUndoState_));
    textUndoPushed_ = true;
}

void App::applyTextEditChange() {
    if (!textEditing_ || textEditIndex_ >= annotations_.size()) return;
    pushTextEditUndoOnce();
    markEdited();
    refreshTextEditSpec();
}

void App::refreshTextEditSpec() {
    if (!textEditing_ || textEditIndex_ >= annotations_.size()) return;
    AnnotationSpec& spec = annotations_[textEditIndex_];
    spec.text = textEditDisplayText();
    spec.styles = textEditDisplayStyles();
    // 空になったら枠は縮めない(利用者が決めた大きさのまま入力を続けられるように)。
    // 失敗しても枠が古いだけで編集は続けられる
    if (!spec.text.empty()) measureTextHeight(spec);
    notifyCaretMoved();
    host_.requestRedraw();
}

std::string App::textEditDisplayText() const {
    if (composition_.empty()) return textBuffer_.text();
    std::string out = textBuffer_.text();
    out.insert(textBuffer_.caret(), composition_);
    return out;
}

std::vector<TextStyleRun> App::textEditDisplayStyles() const {
    std::vector<TextStyleRun> styles = textBuffer_.styles();
    // 変換中文字列を挿入した分だけ後ろの書式をずらす(挿入と同じ扱い)
    if (!composition_.empty()) {
        adjustTextStyles(styles, textBuffer_.caret(), 0, composition_.size());
    }
    return styles;
}

size_t App::textEditCaretOffset() const {
    return textBuffer_.caret() + (composition_.empty() ? 0 : compositionCaret_);
}

void App::resetComposition() {
    composition_.clear();
    compositionCaret_ = 0;
    compositionTargetBegin_ = 0;
    compositionTargetEnd_ = 0;
}

void App::beginComposition() {
    if (!textEditing_) return;
    // 変換は選択範囲を置き換える。先に消してキャレットを 1 点にしておく
    if (textBuffer_.deleteSelection()) applyTextEditChange();
}

void App::setComposition(const std::string& utf8, size_t caretBytes, size_t targetBegin,
                         size_t targetEnd) {
    if (!textEditing_) return;
    composition_ = utf8;
    compositionCaret_ = std::min(caretBytes, composition_.size());
    compositionTargetBegin_ = std::min(targetBegin, composition_.size());
    compositionTargetEnd_ = std::clamp(targetEnd, compositionTargetBegin_, composition_.size());
    refreshTextEditSpec();
}

void App::clearComposition() {
    if (composition_.empty()) return;
    resetComposition();
    refreshTextEditSpec();
}

void App::notifyCaretMoved() {
    textEditCaretOn_ = true;  // 移動直後は必ず見えている状態から点滅を始める
    if (!textEditing_ || textEditIndex_ >= annotations_.size()) return;
    const AnnotationSpec& spec = annotations_[textEditIndex_];
    const TextCaretMetrics caret = rasterizer_.caretMetrics(
        spec, utf8ToUtf16Offset(spec.text, textEditCaretOffset()));
    const BoundsF bounds = annotationBounds(spec);
    // 回転後の見た目の位置へ合わせる(IME 変換ウィンドウはスクリーン座標で置く)
    const Point local{bounds.minX + caret.x, bounds.minY + caret.y};
    const Point center = annotationCenter(spec);
    const Point rotated = rotateAround(local, center, spec.angleDeg);
    const Point screen = imageToScreen().apply(rotated);
    host_.setTextEditing(true, screen, caret.height * std::max(viewport_.zoom(), 0.001f));
}

size_t App::textOffsetAt(Point imagePos) const {
    if (textEditIndex_ >= annotations_.size()) return 0;
    const AnnotationSpec& spec = annotations_[textEditIndex_];
    const BoundsF bounds = annotationBounds(spec);
    // 注釈は中心周りに回転して描かれるため、逆回転してから枠内のローカル座標にする
    const Point unrotated = rotateAround(imagePos, annotationCenter(spec), -spec.angleDeg);
    const size_t utf16 = rasterizer_.hitTestTextOffset(spec, unrotated.x - bounds.minX,
                                                       unrotated.y - bounds.minY);
    return utf16ToUtf8Offset(textBuffer_.text(), utf16);
}

void App::moveCaretVertical(bool down, bool extendSelection) {
    if (textEditIndex_ >= annotations_.size()) return;
    const AnnotationSpec& spec = annotations_[textEditIndex_];
    const TextCaretMetrics caret = rasterizer_.caretMetrics(
        spec, utf8ToUtf16Offset(textBuffer_.text(), textBuffer_.caret()));
    if (caret.height <= 0) return;
    // 現在のキャレットの 1 行上/下の中心を叩いて、その表示行の文字位置を得る
    const float y = down ? caret.y + caret.height * 1.5f : caret.y - caret.height * 0.5f;
    const size_t utf16 = rasterizer_.hitTestTextOffset(spec, caret.x, y);
    textBuffer_.setCaret(utf16ToUtf8Offset(textBuffer_.text(), utf16), extendSelection);
}

bool App::handleTextEditKey(const KeyChord& chord) {
    // 変換中のキーは IME が処理する(変換候補の選択・確定・取消)。App は触らない
    if (isComposing()) return true;
    const bool shift = chord.shift;
    if (chord.ctrl && !chord.alt) {
        // 英字キーは KeyCode の列挙子ではない(ASCII をそのまま値に持つ)ため if で比べる
        const auto letter = [&chord](char c) {
            return chord.key == static_cast<KeyCode>(c);
        };
        if (chord.key == KeyCode::Enter) {
            commitTextEdit();  // Ctrl+Enter でも確定できる(Enter は改行のため)
        } else if (letter('A')) {
            textBuffer_.selectAll();
            notifyCaretMoved();
            host_.requestRedraw();
        } else if (letter('C') || letter('X')) {
            if (textBuffer_.hasSelection()) {
                clipboard_.setText(textBuffer_.selectedText());
                if (letter('X')) {
                    textBuffer_.deleteSelection();
                    applyTextEditChange();
                }
            }
        } else if (letter('V')) {
            const std::string pasted = clipboard_.getText();
            if (!pasted.empty()) {
                textBuffer_.insert(pasted);
                applyTextEditChange();
            }
        } else if (letter('B')) {
            // 選択部分の太字トグル(全体が太字なら解除。一般的なエディタと同じ)
            if (textBuffer_.toggleSelectionFlag(TextStyleFlag::Bold)) applyTextEditChange();
        } else if (letter('I')) {
            if (textBuffer_.toggleSelectionFlag(TextStyleFlag::Italic)) applyTextEditChange();
        } else if (letter('U')) {
            if (textBuffer_.toggleSelectionFlag(TextStyleFlag::Underline)) {
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
        textBuffer_.insert("\n");
        applyTextEditChange();
        return true;
    case KeyCode::Backspace:
        if (textBuffer_.backspace()) applyTextEditChange();
        return true;
    case KeyCode::Delete:
        if (textBuffer_.deleteForward()) applyTextEditChange();
        return true;
    case KeyCode::Left:
        textBuffer_.moveLeft(shift);
        break;
    case KeyCode::Right:
        textBuffer_.moveRight(shift);
        break;
    case KeyCode::Up:
        moveCaretVertical(false, shift);
        break;
    case KeyCode::Down:
        moveCaretVertical(true, shift);
        break;
    case KeyCode::Home:
        textBuffer_.moveLineStart(shift);
        break;
    case KeyCode::End:
        textBuffer_.moveLineEnd(shift);
        break;
    case KeyCode::Tab:
        textBuffer_.insert("\t");
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
    if (!textEditing_ || utf8.empty()) return;
    resetComposition();  // 確定文字列が変換中文字列を置き換える
    textBuffer_.insert(utf8);
    applyTextEditChange();
}

bool App::wantsTextCursor(Point screenPos) const {
    if (!textEditing_ || textEditIndex_ >= annotations_.size()) return false;
    const Point imagePos = imageToScreen().inverted().apply(screenPos);
    const float tolerance = kHitTolerancePx / std::max(viewport_.zoom(), 0.001f);
    return hitTestAnnotation(annotations_[textEditIndex_], imagePos, tolerance);
}

void App::onCaretBlink() {
    if (!textEditing_) return;
    textEditCaretOn_ = !textEditCaretOn_;
    host_.requestRedraw();
}

void App::deleteSelectedAnnotation() {
    if (textEditing_) commitTextEdit();  // 編集を確定してから対象を確定させる
    if (!selected_ || *selected_ >= annotations_.size()) {
        showMessage("削除する注釈がありません");
        return;
    }
    pushUndo();
    annotations_.erase(annotations_.begin() + static_cast<std::ptrdiff_t>(*selected_));
    selected_.reset();
    objectDrag_ = ObjectDrag::None;
    markEdited();
    host_.requestRedraw();
}

std::shared_ptr<DecodedImage> App::compositeImage() const {
    if (!current_ || annotations_.empty()) return current_;
    auto out = std::make_shared<DecodedImage>(*current_);  // キャッシュ共有のためコピー
    for (const AnnotationSpec& spec : annotations_) {
        const AnnotationOverlay overlay = rasterizer_.rasterize(spec);
        if (overlay.image) blendOverlay(*out, *overlay.image, overlay.x, overlay.y);
    }
    return out;
}

void App::markEdited() {
    if (edited_) return;
    edited_ = true;
    updateTitle();
}

void App::pushUndo() {
    pushUndoState({current_, annotations_});
}

void App::pushUndoState(UndoState state) {
    undoStack_.push_back(std::move(state));
    if (undoStack_.size() > kUndoLimit) undoStack_.erase(undoStack_.begin());
}

void App::pushDragUndoOnce() {
    if (dragUndoPushed_) return;
    pushUndo();
    dragUndoPushed_ = true;
}

void App::executeUndo() {
    if (textEditing_) commitTextEdit();  // 編集中の内容を確定してから履歴を戻す
    if (undoStack_.empty()) {
        showMessage("取り消す編集はありません");
        return;
    }
    UndoState& state = undoStack_.back();
    const bool sizeChanged =
        current_ && state.image &&
        (current_->width != state.image->width || current_->height != state.image->height);
    current_ = std::move(state.image);
    annotations_ = std::move(state.annotations);
    undoStack_.pop_back();
    selected_.reset();  // index が指す対象が変わりうるため選択は解除する
    objectDrag_ = ObjectDrag::None;
    // トリミングの取り消しでサイズが戻るときだけビューを再設定する(回転等を保つ)
    if (sizeChanged) {
        viewport_.setImage(
            {static_cast<float>(current_->width), static_cast<float>(current_->height)});
    }
    edited_ = !undoStack_.empty();
    updateTitle();
    host_.requestRedraw();
}

void App::discardEdits() {
    if (textEditing_) {
        // 画像が変わるため注釈ごと捨てられる。host へ編集終了だけ伝えて状態を落とす
        textEditing_ = false;
        textEditMouseSelect_ = false;
        host_.setTextEditing(false, {}, 0);
    }
    selecting_ = false;
    selected_.reset();
    objectDrag_ = ObjectDrag::None;
    annotations_.clear();
    if (undoStack_.empty() && !edited_) return;
    undoStack_.clear();
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
    // 直線・矢印は正方形の bbox = 45 度固定になってしまうので対象外
    if (!shift || tool_ == EditTool::Line || tool_ == EditTool::Arrow) return p;
    return constrainToSquare(selStartImage_, p);
}

SelectionView App::selection() const {
    SelectionView sel;
    // 図形ツールはプレビューで実物を描くので、ラバーバンドは出さない
    sel.visible = selecting_ && current_ != nullptr && !previewVisible();
    if (!sel.visible) return sel;
    const Matrix3x2 m = imageToScreen();
    sel.p1 = m.apply(selStartImage_);
    sel.p2 = m.apply(selEndImage_);
    sel.borderRGB = 0x3399FF;
    sel.fillARGB = 0x303399FF;  // 半透明の塗り
    return sel;
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
    if (textEditing_ && textEditIndex_ < annotations_.size()) {
        const AnnotationSpec& spec = annotations_[textEditIndex_];
        const BoundsF bounds = annotationBounds(spec);
        TextEditView& edit = view.textEdit;
        edit.active = true;
        edit.index = textEditIndex_;
        edit.caretVisible = textEditCaretOn_ && !textBuffer_.hasSelection();
        // spec.text は変換中文字列を混ぜた表示用テキスト。位置指定はこれを基準にする
        const std::string& text = spec.text;
        const TextCaretMetrics caret =
            rasterizer_.caretMetrics(spec, utf8ToUtf16Offset(text, textEditCaretOffset()));
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
        if (textBuffer_.hasSelection()) {
            edit.selectionRects =
                rectsFor(textBuffer_.selectionBegin(), textBuffer_.selectionEnd());
        }
        if (!composition_.empty()) {
            const size_t base = textBuffer_.caret();  // 変換中文字列はここに挿入されている
            edit.compositionRects = rectsFor(base, base + composition_.size());
            if (compositionTargetEnd_ > compositionTargetBegin_) {
                edit.compositionTargetRects = rectsFor(base + compositionTargetBegin_,
                                                       base + compositionTargetEnd_);
            }
        }
        edit.caretRGB = 0x3399FF;
        edit.selectionARGB = 0x603399FF;
    }
    return view;
}

void App::onDragPan(float dx, float dy) {
    viewport_.panBy(dx, dy);
    host_.requestRedraw();
}

void App::onMouseMove(Point screenPos, bool shift) {
    // テキスト編集中の左ドラッグは範囲選択(キャレット側だけを動かす)
    if (textEditMouseSelect_) {
        const Point imagePos = imageToScreen().inverted().apply(screenPos);
        textBuffer_.setCaret(textOffsetAt(imagePos), true);
        notifyCaretMoved();
        host_.requestRedraw();
        return;
    }
    // 注釈の移動・回転ドラッグ中はホバー表示より優先する
    if (objectDrag_ != ObjectDrag::None && selected_ && *selected_ < annotations_.size()) {
        AnnotationSpec& spec = annotations_[*selected_];
        if (objectDrag_ == ObjectDrag::Move) {
            const Point imagePos = imageToScreen().inverted().apply(screenPos);
            const float dx = imagePos.x - dragStartImage_.x;
            const float dy = imagePos.y - dragStartImage_.y;
            pushDragUndoOnce();
            spec.p1 = {dragOrigSpec_.p1.x + dx, dragOrigSpec_.p1.y + dy};
            spec.p2 = {dragOrigSpec_.p2.x + dx, dragOrigSpec_.p2.y + dy};
        } else if (objectDrag_ == ObjectDrag::Rotate) {
            const Point center = imageToScreen().apply(annotationCenter(spec));
            float angle = dragOrigSpec_.angleDeg + angleDegFrom(center, screenPos) -
                          dragStartAngleDeg_;
            if (shift) angle = snapAngleDeg(angle, kAngleSnapDeg);
            pushDragUndoOnce();
            spec.angleDeg = normalizeAngleDeg(angle);
        } else {
            const Point imagePos = imageToScreen().inverted().apply(screenPos);
            pushDragUndoOnce();
            const AnnotationSpec resized =
                resizeAnnotation(dragOrigSpec_, dragResizeHandle_, imagePos, shift);
            spec.p1 = resized.p1;
            spec.p2 = resized.p2;
        }
        markEdited();
        host_.requestRedraw();
        return;
    }
    std::string text = hoverInfoText(screenPos);
    if (text == hoverText_) return;  // 表示が変わるときだけ再描画する
    hoverText_ = std::move(text);
    if (statusBarVisible()) host_.requestRedraw();
}

void App::onMouseLeave() {
    if (hoverText_.empty()) return;
    hoverText_.clear();
    if (statusBarVisible()) host_.requestRedraw();
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
    if (displayedPath_ == list_.current() && (current_ || loadFailed_)) return;
    refreshCurrent();
}

void App::refreshCurrent() {
    discardEdits();
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
    return sidebarEnabled_ && !host_.isFullscreen();
}

float App::sidebarOffset() const {
    if (!sidebarVisible()) return 0.0f;
    // 操作一覧は「操作名 + キー」が入りきらないと読めないので、狭い設定でも広げる
    return sidebarMode_ == SidebarMode::Help ? std::max(sidebarWidth_, kHelpSidebarWidth)
                                             : sidebarWidth_;
}

size_t App::sidebarItemCount() const {
    return sidebarMode_ == SidebarMode::Help ? helpLines_.size() : list_.size();
}

float App::sidebarViewHeight() const {
    const float barHeight = statusBarVisible() ? kStatusBarHeight : 0.0f;
    return std::max(clientSize_.h - barHeight, 1.0f);
}

void App::clampSidebarScroll() {
    const float maxScroll = std::max(
        0.0f,
        static_cast<float>(sidebarItemCount()) * kSidebarItemHeight - sidebarViewHeight());
    sidebarScroll_ = std::clamp(sidebarScroll_, 0.0f, maxScroll);
}

void App::scrollSidebarToCurrent() {
    if (sidebarMode_ == SidebarMode::Help) return;  // 操作一覧に「現在項目」はない
    if (list_.empty()) {
        sidebarScroll_ = 0;
        return;
    }
    const float itemTop = static_cast<float>(list_.index()) * kSidebarItemHeight;
    const float viewHeight = sidebarViewHeight();
    if (itemTop < sidebarScroll_) {
        sidebarScroll_ = itemTop;
    } else if (itemTop + kSidebarItemHeight > sidebarScroll_ + viewHeight) {
        sidebarScroll_ = itemTop + kSidebarItemHeight - viewHeight;
    }
    clampSidebarScroll();
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
        // 右ドラッグが何をするかは見た目に出ないので、現在のツールをここに出す
        bar.leftText = std::format("{} x {} px  |  ツール: {}", current_->width,
                                   current_->height, toolLabel(tool_));
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
    sb.itemHeight = kSidebarItemHeight;
    sb.backgroundRGB = darkTheme_ ? 0x252526 : 0xF3F3F3;
    sb.textRGB = darkTheme_ ? 0xCCCCCC : 0x333333;
    sb.currentBackgroundRGB = darkTheme_ ? 0x094771 : 0xCCE4F7;
    sb.currentTextRGB = darkTheme_ ? 0xFFFFFF : 0x1A1A1A;
    sb.scrollbarRGB = darkTheme_ ? 0x666666 : 0xA0A0A0;
    sb.scrollOffset = sidebarScroll_;
    const size_t count = sidebarItemCount();
    sb.contentHeight = static_cast<float>(count) * kSidebarItemHeight;

    // 可視範囲の項目だけを渡す(先頭が部分的に隠れる分は firstItemY が負になる)
    const size_t first = static_cast<size_t>(sidebarScroll_ / kSidebarItemHeight);
    sb.firstItemY = static_cast<float>(first) * kSidebarItemHeight - sidebarScroll_;
    const size_t maxVisible = static_cast<size_t>(sb.height / kSidebarItemHeight) + 2;
    for (size_t i = first; i < count && i < first + maxVisible; ++i) {
        if (sidebarMode_ == SidebarMode::Help) {
            // 見出し行を current の強調表示で描く(レンダラは両モードを区別しない)
            sb.items.push_back({helpLines_[i].text, helpLines_[i].header});
        } else {
            sb.items.push_back({pathToUtf8(list_.at(i).filename()), i == list_.index()});
        }
    }
    return sb;
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
