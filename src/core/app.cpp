#include "core/app.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <format>

#include "core/annotation_edit.h"
#include "core/edit.h"
#include "core/pixel_convert.h"
#include "core/unicode.h"

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
        if (selected_) {
            selected_.reset();
            objectDrag_ = ObjectDrag::None;
            host_.requestRedraw();
        } else if (selecting_) {
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
    if (sidebarVisible() && screenPos.x < sidebarOffset()) {
        // ステータスバー上(サイドバー領域外)はどちらの操作でもないため消費だけする
        if (screenPos.y >= 0 && screenPos.y < sidebarViewHeight()) {
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
    // 注釈本体 → 選択して移動ドラッグ開始。外れたら選択解除してパンに回す
    const Point imagePos = imageToScreen().inverted().apply(screenPos);
    const float tolerance = kHitTolerancePx / std::max(viewport_.zoom(), 0.001f);
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
    // テキストの高さは内容で決まるため、リサイズ確定時に折り返し後の実寸へ揃える
    if (objectDrag_ == ObjectDrag::Resize && dragUndoPushed_ && selected_ &&
        *selected_ < annotations_.size() &&
        annotations_[*selected_].kind == AnnotationSpec::Kind::Text) {
        if (measureTextExtent(annotations_[*selected_])) host_.requestRedraw();
    }
    objectDrag_ = ObjectDrag::None;
    dragUndoPushed_ = false;
}

bool App::onDoubleClick(Point screenPos) {
    if (!current_) return false;
    if (sidebarVisible() && screenPos.x < sidebarOffset()) return false;
    const Point imagePos = imageToScreen().inverted().apply(screenPos);
    const float tolerance = kHitTolerancePx / std::max(viewport_.zoom(), 0.001f);
    const auto hit = hitTestAnnotations(annotations_, imagePos, tolerance);
    if (!hit || annotations_[*hit].kind != AnnotationSpec::Kind::Text) return false;
    selected_ = hit;
    objectDrag_ = ObjectDrag::None;
    host_.requestRedraw();
    editAnnotationText(*hit);
    return true;
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
        // 単なる右クリック(移動量が小さい): 注釈の上ならオブジェクトメニューを表示する
        selecting_ = false;
        const Point imagePos = imageToScreen().inverted().apply(screenPos);
        const float tolerance = kHitTolerancePx / std::max(viewport_.zoom(), 0.001f);
        if (const auto hit = hitTestAnnotations(annotations_, imagePos, tolerance)) {
            selected_ = hit;
            host_.requestRedraw();
            showObjectMenu(screenPos);
        } else {
            host_.requestRedraw();
        }
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
    const auto leaf = [&entries](std::string text, EditMenuEntry entry, bool checked = false) {
        entries.push_back(entry);
        MenuItem item;
        item.text = std::move(text);
        item.checked = checked;
        return item;
    };
    const auto annotate = [&leaf](std::string text, AnnotationSpec::Kind kind) {
        return leaf(std::move(text), {EditMenuEntry::Action::Annotate, kind, 0});
    };
    using Action = EditMenuEntry::Action;

    std::vector<MenuItem> items;
    items.push_back(leaf("トリミング", {Action::Crop}));
    items.push_back(menuSeparator());
    items.push_back(annotate("矩形", AnnotationSpec::Kind::Rect));
    items.push_back(annotate("楕円", AnnotationSpec::Kind::Ellipse));
    items.push_back(annotate("矢印", AnnotationSpec::Kind::Arrow));
    items.push_back(annotate("直線", AnnotationSpec::Kind::Line));
    items.push_back(annotate("テキスト", AnnotationSpec::Kind::Text));
    items.push_back(menuSeparator());

    MenuItem stroke;
    stroke.text = std::format("線の太さ ({}px)", static_cast<int>(editStrokeWidth_));
    for (const int w : {1, 2, 3, 5, 8, 12, 20}) {
        stroke.children.push_back(
            leaf(std::format("{}px", w),
                 {Action::StrokeWidth, AnnotationSpec::Kind::Rect, static_cast<float>(w)},
                 static_cast<float>(w) == editStrokeWidth_));
    }
    items.push_back(std::move(stroke));

    MenuItem font;
    font.text = std::format("文字サイズ ({}px)", static_cast<int>(editFontSize_));
    for (const int s : {12, 14, 18, 24, 36, 48, 72}) {
        font.children.push_back(
            leaf(std::format("{}px", s),
                 {Action::FontSize, AnnotationSpec::Kind::Rect, static_cast<float>(s)},
                 static_cast<float>(s) == editFontSize_));
    }
    items.push_back(std::move(font));

    items.push_back(
        leaf(std::format("色の変更... (#{:06X})", editColorRGB_), {Action::PickColor}));
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
        items.push_back(leaf("テキストを編集...", {Action::EditText}));
    }
    items.push_back(leaf("削除", {Action::Delete}));
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
        editAnnotationText(*selected_);
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
    case ObjectMenuEntry::Action::PickColor: {
        const auto rgb = host_.showColorPicker(spec.colorRGB);
        if (!rgb || *rgb == spec.colorRGB) return;
        pushUndo();
        spec.colorRGB = *rgb;
        break;
    }
    }
    markEdited();
    host_.requestRedraw();
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
    // 注釈はオブジェクトのまま維持し、切り出した原点ぶんだけ平行移動する
    for (AnnotationSpec& spec : annotations_) {
        translateAnnotation(spec, static_cast<float>(-x0), static_cast<float>(-y0));
    }
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
    // 線幅・文字サイズは「画面上での見た目」基準で画像座標へ換算する
    const float zoom = std::max(viewport_.zoom(), 0.001f);
    spec.strokeWidth = std::max(1.0f, editStrokeWidth_ / zoom);
    spec.fontSize = std::max(4.0f, editFontSize_ / zoom);
    if (kind == AnnotationSpec::Kind::Text) {
        const auto text = host_.showTextInput({});
        if (!text || text->empty()) return;
        spec.text = *text;
        if (!measureTextExtent(spec)) {
            showMessage("描画に失敗しました");
            return;
        }
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
    constexpr float kTextMargin = 2.0f;  // ラスタライザのテキスト用余白と同じ
    const float w =
        std::max(static_cast<float>(overlay.image->width) - kTextMargin * 2, 1.0f);
    const float h =
        std::max(static_cast<float>(overlay.image->height) - kTextMargin * 2, 1.0f);
    const Point origin{std::min(spec.p1.x, spec.p2.x), std::min(spec.p1.y, spec.p2.y)};
    spec.p1 = origin;
    spec.p2 = {origin.x + w, origin.y + h};
    return true;
}

void App::editAnnotationText(size_t index) {
    if (index >= annotations_.size()) return;
    const auto text = host_.showTextInput(annotations_[index].text);
    if (!text || text->empty() || *text == annotations_[index].text) return;
    AnnotationSpec updated = annotations_[index];
    updated.text = *text;
    if (!measureTextExtent(updated)) {
        showMessage("描画に失敗しました");
        return;
    }
    pushUndo();
    annotations_[index] = std::move(updated);
    markEdited();
    host_.requestRedraw();
}

void App::deleteSelectedAnnotation() {
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
    undoStack_.push_back({current_, annotations_});
    if (undoStack_.size() > kUndoLimit) undoStack_.erase(undoStack_.begin());
}

void App::pushDragUndoOnce() {
    if (dragUndoPushed_) return;
    pushUndo();
    dragUndoPushed_ = true;
}

void App::executeUndo() {
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

AnnotationsView App::annotations() const {
    AnnotationsView view;
    view.specs = &annotations_;
    if (selected_ && *selected_ < annotations_.size()) view.selected = selected_;
    view.selectionRGB = 0x3399FF;
    view.handleOffsetPx = kRotationHandleOffsetPx;
    view.handleRadiusPx = kRotationHandleRadiusPx;
    view.resizeHandleSizePx = kResizeHandleSizePx;
    return view;
}

void App::onDragPan(float dx, float dy) {
    viewport_.panBy(dx, dy);
    host_.requestRedraw();
}

void App::onMouseMove(Point screenPos, bool shift) {
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
        bar.leftText = std::format("{} x {} px", current_->width, current_->height);
    } else if (loadFailed_) {
        bar.leftText = "読み込み失敗";
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
        sb.items.push_back({pathToUtf8(list_.at(i).filename()), i == list_.index()});
    }
    return sb;
}

void App::updatePrefetch() {
    cache_.setPrefetch(list_.prefetchOrder(prefetchRadius_));
}

void App::updateTitle() {
    if (clipboardImage_) {
        host_.setTitle(std::format("(クリップボード){} {}% - Blinker",
                                   edited_ ? " (編集済み)" : "",
                                   static_cast<int>(std::lround(viewport_.zoom() * 100))));
        return;
    }
    if (list_.empty()) {
        host_.setTitle("Blinker");
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
    title += " - Blinker";
    host_.setTitle(title);
}

} // namespace blinker
