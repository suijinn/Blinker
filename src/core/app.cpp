#include "core/app.h"

#include <algorithm>
#include <cmath>
#include <format>

#include "core/edit.h"
#include "core/pixel_convert.h"

namespace blinker {

namespace fs = std::filesystem;

namespace {

constexpr unsigned kMessageDurationMs = 3000;

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
    editColorRGB_ = config.getColorRGB("edit", "color", editColorRGB_);
    editStrokeWidth_ = static_cast<float>(std::clamp(
        config.getInt("edit", "stroke_width", static_cast<int>(editStrokeWidth_)), 1, 100));
    editFontSize_ = static_cast<float>(
        std::clamp(config.getInt("edit", "font_size", static_cast<int>(editFontSize_)), 6, 200));
    applyLayout();
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
            showMessage(L"コピーする画像がありません");
        } else if (clipboard_.setImage(*current_)) {
            showMessage(L"画像をクリップボードにコピーしました");
        } else {
            showMessage(L"画像のコピーに失敗しました");
        }
        break;
    case Command::CopyPath:
        if (clipboardImage_ || list_.empty()) {
            showMessage(L"コピーするパスがありません");
        } else if (clipboard_.setText(list_.current().wstring())) {
            showMessage(L"パスをコピーしました: " + list_.current().wstring());
        } else {
            showMessage(L"パスのコピーに失敗しました");
        }
        break;
    case Command::PasteImage:
        if (auto image = clipboard_.getImage(); image && image->width > 0 && image->height > 0) {
            discardEdits();
            current_ = std::move(image);
            clipboardImage_ = true;
            displayedPath_.clear();  // 一覧に戻ったとき必ず再取得させる
            loadFailed_ = false;
            viewport_.setImage(
                {static_cast<float>(current_->width), static_cast<float>(current_->height)});
            updateTitle();
            host_.requestRedraw();
        } else {
            showMessage(L"クリップボードに画像がありません");
        }
        break;
    case Command::SaveImageAs: {
        if (!current_) {
            showMessage(L"保存する画像がありません");
            break;
        }
        const std::wstring defaultName = clipboardImage_ || list_.empty()
                                             ? L"クリップボード.png"
                                             : list_.current().stem().wstring() + L".png";
        if (const auto path = host_.showSaveDialog(defaultName)) {
            if (encoder_.encode(*current_, *path)) {
                showMessage(L"保存しました: " + path->wstring());
            } else {
                showMessage(L"保存に失敗しました: " + path->wstring());
            }
        }
        break;
    }
    case Command::Undo:
        executeUndo();
        break;
    case Command::ToggleSidebar:
        sidebarEnabled_ = !sidebarEnabled_;
        applyLayout();
        if (sidebarEnabled_) scrollSidebarToCurrent();
        onViewChanged();  // フィット再計算でズーム率表示が変わりうる
        break;
    case Command::ToggleStatusBar:
        statusBarEnabled_ = !statusBarEnabled_;
        applyLayout();
        onViewChanged();  // フィット再計算でズーム率表示が変わりうる
        break;
    case Command::Escape:
        if (selecting_) {
            selecting_ = false;
            host_.requestRedraw();
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
    const Command command = keymap_.find(chord);
    if (command == Command::None) return false;
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
    if (!sidebarVisible() || screenPos.x >= sidebarOffset()) return false;
    // ステータスバー上(サイドバー領域外)はどちらの操作でもないため消費だけする
    if (screenPos.y >= 0 && screenPos.y < sidebarViewHeight()) {
        const size_t index =
            static_cast<size_t>((screenPos.y + sidebarScroll_) / kSidebarItemHeight);
        if (index < list_.size() && (list_.jumpTo(index) || clipboardImage_)) refreshCurrent();
    }
    return true;  // サイドバー上のクリックはパン開始にしない
}

void App::onRightDragStart(Point screenPos) {
    if (!current_) return;
    // サイドバー・ステータスバー上からは開始しない
    const float barHeight = statusBarVisible() ? kStatusBarHeight : 0.0f;
    if (screenPos.x < sidebarOffset() || screenPos.y >= clientSize_.h - barHeight) return;
    selecting_ = true;
    selStartScreen_ = screenPos;
    selStartImage_ = clampToImage(imageToScreen().inverted().apply(screenPos));
    selEndImage_ = selStartImage_;
    host_.requestRedraw();
}

void App::onRightDragMove(Point screenPos) {
    if (!selecting_) return;
    selEndImage_ = clampToImage(imageToScreen().inverted().apply(screenPos));
    host_.requestRedraw();
}

void App::onRightDragEnd(Point screenPos) {
    if (!selecting_) return;
    selEndImage_ = clampToImage(imageToScreen().inverted().apply(screenPos));
    const float dx = screenPos.x - selStartScreen_.x;
    const float dy = screenPos.y - selStartScreen_.y;
    if (dx * dx + dy * dy < kDragThresholdPx * kDragThresholdPx) {
        selecting_ = false;  // 単なる右クリック(移動量が小さい)は何もしない
        host_.requestRedraw();
        return;
    }
    // メニュー表示中(モーダル)もラバーバンドは描画されたまま残る。
    // 設定系の項目(太さ・サイズ・回転・色)を選んだ場合は選択領域を保ったまま
    // メニューを再表示し、続けて編集を選べるようにする
    while (true) {
        std::vector<EditMenuEntry> entries;
        const std::vector<MenuItem> items = buildEditMenu(entries);
        const auto choice = host_.showContextMenu(items, screenPos);
        if (!choice || *choice >= entries.size()) break;
        if (applyEditChoice(entries[*choice])) break;
    }
    selecting_ = false;
    host_.requestRedraw();
}

std::vector<MenuItem> App::buildEditMenu(std::vector<EditMenuEntry>& entries) const {
    const auto leaf = [&entries](std::wstring text, EditMenuEntry entry, bool checked = false) {
        entries.push_back(entry);
        MenuItem item;
        item.text = std::move(text);
        item.checked = checked;
        return item;
    };
    const auto annotate = [&leaf](std::wstring text, AnnotationSpec::Kind kind) {
        return leaf(std::move(text), {EditMenuEntry::Action::Annotate, kind, 0});
    };
    using Action = EditMenuEntry::Action;

    std::vector<MenuItem> items;
    items.push_back(leaf(L"トリミング", {Action::Crop}));
    items.push_back({.separator = true});
    items.push_back(annotate(L"矩形", AnnotationSpec::Kind::Rect));
    items.push_back(annotate(L"楕円", AnnotationSpec::Kind::Ellipse));
    items.push_back(annotate(L"矢印", AnnotationSpec::Kind::Arrow));
    items.push_back(annotate(L"直線", AnnotationSpec::Kind::Line));
    items.push_back(annotate(L"テキスト", AnnotationSpec::Kind::Text));
    items.push_back({.separator = true});

    MenuItem stroke;
    stroke.text = std::format(L"線の太さ ({}px)", static_cast<int>(editStrokeWidth_));
    for (const int w : {1, 2, 3, 5, 8, 12, 20}) {
        stroke.children.push_back(
            leaf(std::format(L"{}px", w),
                 {Action::StrokeWidth, AnnotationSpec::Kind::Rect, static_cast<float>(w)},
                 static_cast<float>(w) == editStrokeWidth_));
    }
    items.push_back(std::move(stroke));

    MenuItem font;
    font.text = std::format(L"文字サイズ ({}px)", static_cast<int>(editFontSize_));
    for (const int s : {12, 14, 18, 24, 36, 48, 72}) {
        font.children.push_back(
            leaf(std::format(L"{}px", s),
                 {Action::FontSize, AnnotationSpec::Kind::Rect, static_cast<float>(s)},
                 static_cast<float>(s) == editFontSize_));
    }
    items.push_back(std::move(font));

    MenuItem angle;
    angle.text = std::format(L"回転角度 ({}°)", static_cast<int>(editAngleDeg_));
    for (const int a : {0, 15, 30, 45, 90, 135, 180, 270}) {
        angle.children.push_back(
            leaf(std::format(L"{}°", a),
                 {Action::Angle, AnnotationSpec::Kind::Rect, static_cast<float>(a)},
                 static_cast<float>(a) == editAngleDeg_));
    }
    items.push_back(std::move(angle));

    items.push_back(
        leaf(std::format(L"色の変更... (#{:06X})", editColorRGB_), {Action::PickColor}));
    return items;
}

bool App::applyEditChoice(const EditMenuEntry& entry) {
    switch (entry.action) {
    case EditMenuEntry::Action::Crop:
        applyCrop();
        return true;
    case EditMenuEntry::Action::Annotate:
        applyAnnotation(entry.kind);
        return true;
    case EditMenuEntry::Action::StrokeWidth:
        editStrokeWidth_ = entry.value;
        return false;
    case EditMenuEntry::Action::FontSize:
        editFontSize_ = entry.value;
        return false;
    case EditMenuEntry::Action::Angle:
        editAngleDeg_ = entry.value;
        return false;
    case EditMenuEntry::Action::PickColor:
        if (const auto rgb = host_.showColorPicker(editColorRGB_)) editColorRGB_ = *rgb;
        return false;
    }
    return true;
}

void App::applyCrop() {
    if (!current_) return;
    // 部分的にかかったピクセルも含める(floor/ceil)
    const int x0 = static_cast<int>(std::floor(std::min(selStartImage_.x, selEndImage_.x)));
    const int y0 = static_cast<int>(std::floor(std::min(selStartImage_.y, selEndImage_.y)));
    const int x1 = static_cast<int>(std::ceil(std::max(selStartImage_.x, selEndImage_.x)));
    const int y1 = static_cast<int>(std::ceil(std::max(selStartImage_.y, selEndImage_.y)));
    auto cropped = cropImage(*current_, {x0, y0, x1 - x0, y1 - y0});
    if (!cropped) return;
    pushUndo();
    current_ = std::move(cropped);
    viewport_.setImage(
        {static_cast<float>(current_->width), static_cast<float>(current_->height)});
    edited_ = true;
    updateTitle();
}

void App::applyAnnotation(AnnotationSpec::Kind kind) {
    if (!current_) return;
    AnnotationSpec spec;
    spec.kind = kind;
    spec.p1 = selStartImage_;
    spec.p2 = selEndImage_;
    spec.colorRGB = editColorRGB_;
    spec.angleDeg = editAngleDeg_;
    // 線幅・文字サイズは「画面上での見た目」基準で画像座標へ換算する
    const float zoom = std::max(viewport_.zoom(), 0.001f);
    spec.strokeWidth = std::max(1.0f, editStrokeWidth_ / zoom);
    spec.fontSize = std::max(4.0f, editFontSize_ / zoom);
    if (kind == AnnotationSpec::Kind::Text) {
        const auto text = host_.showTextInput();
        if (!text || text->empty()) return;
        spec.text = *text;
    }
    const AnnotationOverlay overlay = rasterizer_.rasterize(spec);
    if (!overlay.image) {
        showMessage(L"描画に失敗しました");
        return;
    }
    pushUndo();
    auto edited = std::make_shared<DecodedImage>(*current_);  // キャッシュ共有のためコピー
    blendOverlay(*edited, *overlay.image, overlay.x, overlay.y);
    current_ = std::move(edited);
    edited_ = true;
    updateTitle();
}

void App::pushUndo() {
    undoStack_.push_back(current_);
    if (undoStack_.size() > kUndoLimit) undoStack_.erase(undoStack_.begin());
}

void App::executeUndo() {
    if (undoStack_.empty()) {
        showMessage(L"取り消す編集はありません");
        return;
    }
    const bool sizeChanged = current_ && (current_->width != undoStack_.back()->width ||
                                          current_->height != undoStack_.back()->height);
    current_ = std::move(undoStack_.back());
    undoStack_.pop_back();
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
    selecting_ = false;
    if (undoStack_.empty() && !edited_) return;
    undoStack_.clear();
    if (edited_) {
        edited_ = false;
        showMessage(L"編集を破棄しました");
    }
}

Point App::clampToImage(Point imagePos) const {
    if (!current_) return imagePos;
    return {std::clamp(imagePos.x, 0.0f, static_cast<float>(current_->width)),
            std::clamp(imagePos.y, 0.0f, static_cast<float>(current_->height))};
}

SelectionView App::selection() const {
    SelectionView sel;
    sel.visible = selecting_ && current_ != nullptr;
    if (!sel.visible) return sel;
    const Matrix3x2 m = imageToScreen();
    sel.p1 = m.apply(selStartImage_);
    sel.p2 = m.apply(selEndImage_);
    sel.borderRGB = 0x3399FF;
    sel.fillARGB = 0x303399FF;  // 半透明の塗り
    return sel;
}

void App::onDragPan(float dx, float dy) {
    viewport_.panBy(dx, dy);
    host_.requestRedraw();
}

void App::onMouseMove(Point screenPos) {
    std::wstring text = hoverInfoText(screenPos);
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
        updateTitle();
        host_.requestRedraw();
        return;
    }
    const fs::path& path = list_.current();
    bool failed = false;
    if (auto image = cache_.tryGet(path, &failed)) {
        current_ = std::move(image);
        displayedPath_ = path;
        loadFailed_ = false;
        viewport_.setImage(
            {static_cast<float>(current_->width), static_cast<float>(current_->height)});
    } else if (failed) {
        current_.reset();
        displayedPath_ = path;
        loadFailed_ = true;
    } else {
        // デコード待ち。前の画像を表示したまま onDecodeCompleted を待つ
        cache_.requestNow(path);
        loadFailed_ = false;
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
    return sidebarVisible() ? sidebarWidth_ : 0.0f;
}

float App::sidebarViewHeight() const {
    const float barHeight = statusBarVisible() ? kStatusBarHeight : 0.0f;
    return std::max(clientSize_.h - barHeight, 1.0f);
}

void App::clampSidebarScroll() {
    const float maxScroll = std::max(
        0.0f, static_cast<float>(list_.size()) * kSidebarItemHeight - sidebarViewHeight());
    sidebarScroll_ = std::clamp(sidebarScroll_, 0.0f, maxScroll);
}

void App::scrollSidebarToCurrent() {
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

std::wstring App::hoverInfoText(Point screenPos) const {
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
    std::wstring text = std::format(L"({}, {})  #{:02X}{:02X}{:02X}", x, y, r, g, b);
    text += a == 255 ? std::format(L"  RGB({}, {}, {})", r, g, b)
                     : std::format(L"  RGBA({}, {}, {}, {})", r, g, b, a);
    return text;
}

void App::showMessage(std::wstring text) {
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
        bar.leftText = std::format(L"{} x {} px", current_->width, current_->height);
    } else if (loadFailed_) {
        bar.leftText = L"読み込み失敗";
    }
    bar.rightText = hoverText_;
    return bar;
}

SidebarView App::sidebar() const {
    SidebarView sb;
    sb.visible = sidebarVisible();
    if (!sb.visible) return sb;
    sb.width = sidebarWidth_;
    sb.height = sidebarViewHeight();
    sb.itemHeight = kSidebarItemHeight;
    sb.backgroundRGB = darkTheme_ ? 0x252526 : 0xF3F3F3;
    sb.textRGB = darkTheme_ ? 0xCCCCCC : 0x333333;
    sb.currentBackgroundRGB = darkTheme_ ? 0x094771 : 0xCCE4F7;
    sb.currentTextRGB = darkTheme_ ? 0xFFFFFF : 0x1A1A1A;
    sb.scrollbarRGB = darkTheme_ ? 0x666666 : 0xA0A0A0;
    sb.scrollOffset = sidebarScroll_;
    sb.contentHeight = static_cast<float>(list_.size()) * kSidebarItemHeight;

    // 可視範囲の項目だけを渡す(先頭が部分的に隠れる分は firstItemY が負になる)
    const size_t first = static_cast<size_t>(sidebarScroll_ / kSidebarItemHeight);
    sb.firstItemY = static_cast<float>(first) * kSidebarItemHeight - sidebarScroll_;
    const size_t maxVisible = static_cast<size_t>(sb.height / kSidebarItemHeight) + 2;
    for (size_t i = first; i < list_.size() && i < first + maxVisible; ++i) {
        sb.items.push_back({list_.at(i).filename().wstring(), i == list_.index()});
    }
    return sb;
}

void App::updatePrefetch() {
    cache_.setPrefetch(list_.prefetchOrder(prefetchRadius_));
}

void App::updateTitle() {
    if (clipboardImage_) {
        host_.setTitle(std::format(L"(クリップボード){} {}% - Blinker",
                                   edited_ ? L" (編集済み)" : L"",
                                   static_cast<int>(std::lround(viewport_.zoom() * 100))));
        return;
    }
    if (list_.empty()) {
        host_.setTitle(L"Blinker");
        return;
    }
    std::wstring title = std::format(L"{} [{}/{}]", list_.current().filename().wstring(),
                                     list_.index() + 1, list_.size());
    if (loadFailed_) {
        title += L" (読み込み失敗)";
    } else if (displayedPath_ != list_.current()) {
        title += L" (読み込み中)";
    } else {
        if (edited_) title += L" (編集済み)";
        title += std::format(L" {}%", static_cast<int>(std::lround(viewport_.zoom() * 100)));
    }
    title += L" - Blinker";
    host_.setTitle(title);
}

} // namespace blinker
