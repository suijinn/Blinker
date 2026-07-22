#include "sdl/window_sdl.h"

#include <atomic>
#include <mutex>

#include "core/unicode.h"
#include "platform/image_formats.h"

namespace blinker {
namespace {

// SDL のファイルダイアログのフィルタ書式("ext1;ext2"、ドットなし)
std::string joinExtensionsForFilter() {
    std::string out;
    for (const auto ext : kImageExtensions) {
        if (!out.empty()) out += ';';
        out += ext.substr(1);  // 先頭の '.' を除く
    }
    return out;
}

} // namespace

// コールバックは別スレッドから来る可能性があるため atomic + mutex で受け渡す
struct WindowSdl::DialogState {
    std::mutex mutex;
    std::atomic<bool> done{false};
    std::optional<std::filesystem::path> path;

    static void SDLCALL callback(void* userdata, const char* const* filelist, int) {
        auto* state = static_cast<DialogState*>(userdata);
        {
            const std::lock_guard<std::mutex> lock(state->mutex);
            if (filelist && filelist[0] && filelist[0][0] != '\0') {
                state->path = pathFromUtf8(filelist[0]);
            }
        }
        state->done.store(true, std::memory_order_release);
    }
};

bool WindowSdl::create(FontStb& font) {
    if (!SDL_CreateWindowAndRenderer("Blinker", 1024, 768,
                                     SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY,
                                     &window_, &sdlRenderer_)) {
        return false;
    }
    renderer_ = std::make_unique<RendererSdl>(sdlRenderer_, font);
    const Uint32 base = SDL_RegisterEvents(2);
    if (base == 0) return false;
    eventDecoded_ = base;
    eventTimer_ = base + 1;
    return true;
}

void WindowSdl::attachApp(App* app) {
    app_ = app;
    if (app_) {
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(window_, &w, &h);
        app_->onResize(static_cast<float>(w), static_cast<float>(h));
    }
}

void WindowSdl::postDecodedEvent() {
    SDL_Event event{};
    event.type = eventDecoded_;
    SDL_PushEvent(&event);  // スレッド安全
}

Point WindowSdl::toPixels(float x, float y) const {
    // 高 DPI 環境ではウィンドウ座標と描画ピクセルが異なる。App は物理ピクセルで扱う
    const float scale = SDL_GetWindowPixelDensity(window_);
    return {x * scale, y * scale};
}

void WindowSdl::run() {
    running_ = true;
    renderIfNeeded();
    while (running_) {
        SDL_Event event;
        if (!SDL_WaitEvent(&event)) break;
        handleEvent(event);
        while (SDL_PollEvent(&event)) handleEvent(event);  // まとめて処理してから描画
        renderIfNeeded();
    }
}

void WindowSdl::renderIfNeeded() {
    if (!needsRedraw_ || !app_ || !renderer_) return;
    needsRedraw_ = false;
    renderer_->render(app_->currentImage(), app_->imageToScreen(), app_->zoom(),
                      app_->backgroundRGB(), app_->annotations(), app_->selection(),
                      app_->sidebar(), app_->statusBar());
}

void WindowSdl::handleEvent(const SDL_Event& event) {
    if (event.type == eventDecoded_) {
        if (app_) app_->onDecodeCompleted();
        return;
    }
    if (event.type == eventTimer_) {
        if (app_) app_->onTimer();
        return;
    }
    switch (event.type) {
    case SDL_EVENT_QUIT:
        running_ = false;
        return;
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        if (app_) {
            app_->onResize(static_cast<float>(event.window.data1),
                           static_cast<float>(event.window.data2));
        }
        needsRedraw_ = true;
        return;
    case SDL_EVENT_WINDOW_EXPOSED:
        needsRedraw_ = true;
        return;
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        if (app_) app_->onMouseLeave();
        return;
    case SDL_EVENT_KEY_DOWN: {
        if (!app_) return;
        const KeyCode key = keyCodeFromSdl(event.key.key, event.key.mod);
        if (key == KeyCode::None) return;
        KeyChord chord{key};
        chord.ctrl = (event.key.mod & SDL_KMOD_CTRL) != 0;
        chord.shift = (event.key.mod & SDL_KMOD_SHIFT) != 0;
        chord.alt = (event.key.mod & SDL_KMOD_ALT) != 0;
        app_->onKey(chord);
        return;
    }
    case SDL_EVENT_MOUSE_WHEEL: {
        if (!app_) return;
        const Point pos = toPixels(event.wheel.mouse_x, event.wheel.mouse_y);
        app_->onWheel(event.wheel.y, pos);
        return;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        if (!app_) return;
        const Point pos = toPixels(event.button.x, event.button.y);
        if (event.button.button == SDL_BUTTON_LEFT) {
            if (event.button.clicks == 2 && app_->onDoubleClick(pos)) return;
            SDL_CaptureMouse(true);
            if (!app_->onMouseDown(pos)) {
                dragging_ = true;
                lastDragX_ = pos.x;
                lastDragY_ = pos.y;
            }
        }
        // 右ドラッグ(編集)は SDL バックエンドでは扱わない。ツールの切り替えに必要な
        // ポップアップメニューも注釈の描画も未実装で、見えない注釈だけが増えてしまう
        return;
    }
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        if (!app_) return;
        if (event.button.button == SDL_BUTTON_LEFT) {
            dragging_ = false;
            SDL_CaptureMouse(false);
            app_->onMouseUp();
        }
        return;
    }
    case SDL_EVENT_MOUSE_MOTION: {
        if (!app_) return;
        const Point pos = toPixels(event.motion.x, event.motion.y);
        if (dragging_) {
            app_->onDragPan(pos.x - lastDragX_, pos.y - lastDragY_);
            lastDragX_ = pos.x;
            lastDragY_ = pos.y;
        }
        app_->onMouseMove(pos, (SDL_GetModState() & SDL_KMOD_SHIFT) != 0);
        return;
    }
    case SDL_EVENT_DROP_FILE:
        if (app_ && event.drop.data) app_->openPath(pathFromUtf8(event.drop.data));
        return;
    default:
        return;
    }
}

void WindowSdl::setTitle(const std::string& title) {
    SDL_SetWindowTitle(window_, title.c_str());  // SDL は UTF-8 ネイティブ
}

void WindowSdl::setFullscreen(bool enabled) {
    if (enabled == fullscreen_) return;
    SDL_SetWindowFullscreen(window_, enabled);
    fullscreen_ = enabled;
    needsRedraw_ = true;
}

std::optional<std::filesystem::path> WindowSdl::waitForDialog(DialogState& state) {
    // コールバック完了までイベントを処理しつつ待つ(モーダル相当)。
    // 再入を避けるため、この間の入力イベントは捨て、終了・サイズ変更だけ反映する
    while (!state.done.load(std::memory_order_acquire)) {
        SDL_Event event;
        if (!SDL_WaitEventTimeout(&event, 50)) continue;
        switch (event.type) {
        case SDL_EVENT_QUIT:
            running_ = false;
            state.done.store(true, std::memory_order_release);  // 待ちを打ち切る
            break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_EXPOSED:
            handleEvent(event);
            renderIfNeeded();
            break;
        default:
            break;
        }
    }
    const std::lock_guard<std::mutex> lock(state.mutex);
    return state.path;
}

std::optional<std::filesystem::path> WindowSdl::showOpenDialog() {
    const std::string patterns = joinExtensionsForFilter();
    const SDL_DialogFileFilter filters[] = {
        {"画像ファイル", patterns.c_str()},
        {"すべてのファイル", "*"},
    };
    DialogState state;
    SDL_ShowOpenFileDialog(DialogState::callback, &state, window_, filters, 2, nullptr,
                           false);
    return waitForDialog(state);
}

std::optional<std::filesystem::path> WindowSdl::showSaveDialog(
    const std::string& defaultFileName) {
    // フィルタ順は EncoderStb の対応形式と一致させる
    const SDL_DialogFileFilter filters[] = {
        {"PNG", "png"},
        {"JPEG", "jpg;jpeg"},
        {"BMP", "bmp"},
    };
    DialogState state;
    SDL_ShowSaveFileDialog(DialogState::callback, &state, window_, filters, 3,
                           defaultFileName.c_str());
    auto path = waitForDialog(state);
    if (path && !path->has_extension()) *path += ".png";
    return path;
}

std::optional<size_t> WindowSdl::showContextMenu(const std::vector<MenuItem>&, Point) {
    return std::nullopt;  // 未実装(編集メニュー)。閲覧機能には影響しない
}

void WindowSdl::setTextEditing(bool, Point, float) {
    // 未実装(テキスト注釈)。SDL バックエンドは編集メニューに到達しないため呼ばれない
}

std::optional<uint32_t> WindowSdl::showColorPicker(uint32_t) {
    return std::nullopt;  // 未実装(色選択)
}

void WindowSdl::startTimer(unsigned milliseconds) {
    if (timerId_ != 0) SDL_RemoveTimer(timerId_);
    // タイマーコールバックは別スレッドで走るためイベントで UI スレッドへ渡す
    timerId_ = SDL_AddTimer(
        milliseconds,
        [](void* userdata, SDL_TimerID, Uint32) -> Uint32 {
            auto* self = static_cast<WindowSdl*>(userdata);
            SDL_Event event{};
            event.type = self->eventTimer_;
            SDL_PushEvent(&event);
            return 0;  // 単発
        },
        this);
}

void WindowSdl::quit() {
    SDL_Event event{};
    event.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&event);
}

KeyCode WindowSdl::keyCodeFromSdl(SDL_Keycode key, SDL_Keymod mod) {
    if (key >= SDLK_A && key <= SDLK_Z) {  // SDL の英字キーコードは小文字 ASCII
        return static_cast<KeyCode>('A' + (key - SDLK_A));
    }
    if (key >= SDLK_0 && key <= SDLK_9) {
        return static_cast<KeyCode>(key);
    }
    // テンキー: NumLock オンなら数字、オフならナビゲーションキーとして扱う
    // (Windows が VK_LEFT 等へ変換するのと同じ挙動に揃える)
    if ((mod & SDL_KMOD_NUM) != 0) {
        if (key >= SDLK_KP_1 && key <= SDLK_KP_9) {
            return static_cast<KeyCode>('1' + (key - SDLK_KP_1));
        }
        if (key == SDLK_KP_0) return static_cast<KeyCode>('0');
    } else {
        switch (key) {
        case SDLK_KP_4: return KeyCode::Left;
        case SDLK_KP_6: return KeyCode::Right;
        case SDLK_KP_8: return KeyCode::Up;
        case SDLK_KP_2: return KeyCode::Down;
        case SDLK_KP_7: return KeyCode::Home;
        case SDLK_KP_1: return KeyCode::End;
        case SDLK_KP_9: return KeyCode::PageUp;
        case SDLK_KP_3: return KeyCode::PageDown;
        case SDLK_KP_0: return KeyCode::Insert;
        case SDLK_KP_PERIOD: return KeyCode::Delete;
        default: break;
        }
    }
    if (key >= SDLK_F1 && key <= SDLK_F12) {
        return static_cast<KeyCode>(static_cast<uint16_t>(KeyCode::F1) + (key - SDLK_F1));
    }
    switch (key) {
    case SDLK_LEFT: return KeyCode::Left;
    case SDLK_RIGHT: return KeyCode::Right;
    case SDLK_UP: return KeyCode::Up;
    case SDLK_DOWN: return KeyCode::Down;
    case SDLK_HOME: return KeyCode::Home;
    case SDLK_END: return KeyCode::End;
    case SDLK_PAGEUP: return KeyCode::PageUp;
    case SDLK_PAGEDOWN: return KeyCode::PageDown;
    case SDLK_SPACE: return KeyCode::Space;
    case SDLK_RETURN:
    case SDLK_KP_ENTER: return KeyCode::Enter;
    case SDLK_ESCAPE: return KeyCode::Escape;
    case SDLK_BACKSPACE: return KeyCode::Backspace;
    case SDLK_DELETE: return KeyCode::Delete;
    case SDLK_TAB: return KeyCode::Tab;
    case SDLK_INSERT: return KeyCode::Insert;
    case SDLK_PLUS:
    case SDLK_EQUALS:  // 日本語/英語配列の +/= キー(Win 版の VK_OEM_PLUS 相当)
    case SDLK_KP_PLUS: return KeyCode::Plus;
    case SDLK_MINUS:
    case SDLK_KP_MINUS: return KeyCode::Minus;
    default: return KeyCode::None;
    }
}

} // namespace blinker
