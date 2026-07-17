#include "core/app.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace blinker {

namespace fs = std::filesystem;

namespace {

constexpr unsigned kMessageDurationMs = 3000;

// 事前乗算値からストレート値へ戻す(表示用、四捨五入)
uint8_t unpremultiply(uint8_t value, uint8_t alpha) {
    if (alpha == 0) return 0;
    return static_cast<uint8_t>(std::min(255u, (value * 255u + alpha / 2u) / alpha));
}

} // namespace

App::App(IAppHost& host, IFileSystem& fileSystem, ImageCache& cache, IClipboard& clipboard)
    : host_(host), fileSystem_(fileSystem), cache_(cache), clipboard_(clipboard) {}

void App::applyConfig(const Config& config) {
    keymap_.applyConfig(config.section("keys"));
    backgroundRGB_ = config.getColorRGB("view", "background", backgroundRGB_);
    viewport_.setFitUpscale(config.getBool("view", "fit_upscale", false));
    prefetchRadius_ = std::clamp(config.getInt("view", "prefetch_radius", prefetchRadius_), 0, 8);
    statusBarEnabled_ = config.getBool("view", "statusbar", statusBarEnabled_);
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
    case Command::NextImage:
        if (list_.next()) refreshCurrent();
        break;
    case Command::PrevImage:
        if (list_.prev()) refreshCurrent();
        break;
    case Command::FirstImage:
        if (list_.first()) refreshCurrent();
        break;
    case Command::LastImage:
        if (list_.last()) refreshCurrent();
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
        if (list_.empty()) {
            showMessage(L"コピーするパスがありません");
        } else if (clipboard_.setText(list_.current().wstring())) {
            showMessage(L"パスをコピーしました: " + list_.current().wstring());
        } else {
            showMessage(L"パスのコピーに失敗しました");
        }
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
    viewport_.zoomAt(std::pow(Viewport::kZoomStep, wheelNotches), screenPos);
    onViewChanged();
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
    if (list_.empty()) return;
    // 表示すべき画像がまだ画面に出ていなければ取得を再試行する
    if (displayedPath_ == list_.current() && (current_ || loadFailed_)) return;
    refreshCurrent();
}

void App::refreshCurrent() {
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

void App::applyLayout() {
    const float barHeight = statusBarVisible() ? kStatusBarHeight : 0.0f;
    viewport_.setWindowSize(
        {std::max(clientSize_.w, 1.0f), std::max(clientSize_.h - barHeight, 1.0f)});
}

std::wstring App::hoverInfoText(Point screenPos) const {
    if (!current_) return {};
    // ビューポート外(ステータスバー上を含む)では表示しない
    const float barHeight = statusBarVisible() ? kStatusBarHeight : 0.0f;
    if (screenPos.x < 0 || screenPos.x >= clientSize_.w || screenPos.y < 0 ||
        screenPos.y >= clientSize_.h - barHeight) {
        return {};
    }
    const Point p = viewport_.screenToImage().apply(screenPos);
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

void App::updatePrefetch() {
    cache_.setPrefetch(list_.prefetchOrder(prefetchRadius_));
}

void App::updateTitle() {
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
