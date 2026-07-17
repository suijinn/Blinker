#include "core/app.h"

#include <algorithm>
#include <cmath>
#include <format>

#include "core/pixel_convert.h"

namespace blinker {

namespace fs = std::filesystem;

namespace {

constexpr unsigned kMessageDurationMs = 3000;

} // namespace

App::App(IAppHost& host, IFileSystem& fileSystem, ImageCache& cache, IClipboard& clipboard,
         IImageEncoder& encoder)
    : host_(host),
      fileSystem_(fileSystem),
      cache_(cache),
      clipboard_(clipboard),
      encoder_(encoder) {}

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
        if (host_.isFullscreen()) {
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
    if (list_.empty()) return;
    // 表示すべき画像がまだ画面に出ていなければ取得を再試行する
    if (displayedPath_ == list_.current() && (current_ || loadFailed_)) return;
    refreshCurrent();
}

void App::refreshCurrent() {
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
        host_.setTitle(std::format(L"(クリップボード) {}% - Blinker",
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
        title += std::format(L" {}%", static_cast<int>(std::lround(viewport_.zoom() * 100)));
    }
    title += L" - Blinker";
    host_.setTitle(title);
}

} // namespace blinker
