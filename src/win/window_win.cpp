#include "win/window_win.h"

#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <windowsx.h>

#include "platform/image_formats.h"

namespace blinker {
namespace {

constexpr wchar_t kWindowClass[] = L"BlinkerMainWindow";
constexpr int kIconResourceId = 101;  // blinker.rc の IDI_APPICON

} // namespace

bool MainWindow::create(HINSTANCE hinstance, int showCommand, bool darkTitleBar) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = hinstance;
    wc.hIcon = LoadIconW(hinstance, MAKEINTRESOURCEW(kIconResourceId));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;  // 背景は D2D が毎回塗る(ちらつき防止)
    wc.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&wc)) return false;

    hwnd_ = CreateWindowExW(0, kWindowClass, L"Blinker", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                            CW_USEDEFAULT, 1024, 768, nullptr, nullptr, hinstance, this);
    if (!hwnd_) return false;

    if (darkTitleBar) {
        // 表示前に設定してタイトルバーが白→黒と切り替わるちらつきを防ぐ
        const BOOL dark = TRUE;
        DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    }

    renderer_ = std::make_unique<RendererD2D>(hwnd_);
    DragAcceptFiles(hwnd_, TRUE);
    ShowWindow(hwnd_, showCommand);
    return true;
}

void MainWindow::attachApp(App* app) {
    app_ = app;
    if (app_) {
        // ウィンドウ生成中の WM_SIZE は app 未接続で捨てているため、ここで現在サイズを渡す
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        app_->onResize(static_cast<float>(rc.right), static_cast<float>(rc.bottom));
    }
}

LRESULT CALLBACK MainWindow::wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    MainWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = static_cast<MainWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) return self->handleMessage(msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT MainWindow::handleMessage(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT:
        onPaint();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) onSize(LOWORD(lp), HIWORD(lp));
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (handleKey(wp)) return 0;
        break;  // 未割り当てキーは既定処理へ(Alt+F4 等を殺さない)
    case WM_MOUSEWHEEL: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd_, &pt);
        const float notches = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA;
        if (app_) {
            app_->onWheel(notches,
                          {static_cast<float>(pt.x), static_cast<float>(pt.y)});
        }
        return 0;
    }
    case WM_LBUTTONDOWN:
        dragging_ = true;
        lastDragPos_ = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        SetCapture(hwnd_);
        return 0;
    case WM_MOUSEMOVE:
        if (dragging_ && app_) {
            const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            app_->onDragPan(static_cast<float>(pt.x - lastDragPos_.x),
                            static_cast<float>(pt.y - lastDragPos_.y));
            lastDragPos_ = pt;
        }
        return 0;
    case WM_LBUTTONUP:
        if (dragging_) {
            dragging_ = false;
            ReleaseCapture();
        }
        return 0;
    case WM_DROPFILES:
        onDropFiles(wp);
        return 0;
    case kMsgImageDecoded:
        if (app_) app_->onDecodeCompleted();
        return 0;
    case WM_DPICHANGED: {
        const RECT* suggested = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left, suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_GETMINMAXINFO:
        reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {320, 240};
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

void MainWindow::onPaint() {
    PAINTSTRUCT ps;
    BeginPaint(hwnd_, &ps);
    if (app_ && renderer_) {
        renderer_->render(app_->currentImage(), app_->imageToScreen(), app_->zoom(),
                          app_->backgroundRGB());
    }
    EndPaint(hwnd_, &ps);
}

void MainWindow::onSize(uint32_t width, uint32_t height) {
    if (renderer_) renderer_->resize(width, height);
    if (app_) app_->onResize(static_cast<float>(width), static_cast<float>(height));
    InvalidateRect(hwnd_, nullptr, FALSE);
}

bool MainWindow::handleKey(WPARAM vk) {
    if (!app_) return false;
    const KeyCode key = keyCodeFromVirtualKey(vk);
    if (key == KeyCode::None) return false;
    KeyChord chord{key};
    chord.ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    chord.shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    chord.alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    return app_->onKey(chord);
}

KeyCode MainWindow::keyCodeFromVirtualKey(WPARAM vk) {
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) {
        return static_cast<KeyCode>(vk);
    }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
        return static_cast<KeyCode>('0' + (vk - VK_NUMPAD0));
    }
    if (vk >= VK_F1 && vk <= VK_F12) {
        return static_cast<KeyCode>(static_cast<uint16_t>(KeyCode::F1) + (vk - VK_F1));
    }
    switch (vk) {
    case VK_LEFT: return KeyCode::Left;
    case VK_RIGHT: return KeyCode::Right;
    case VK_UP: return KeyCode::Up;
    case VK_DOWN: return KeyCode::Down;
    case VK_HOME: return KeyCode::Home;
    case VK_END: return KeyCode::End;
    case VK_PRIOR: return KeyCode::PageUp;
    case VK_NEXT: return KeyCode::PageDown;
    case VK_SPACE: return KeyCode::Space;
    case VK_RETURN: return KeyCode::Enter;
    case VK_ESCAPE: return KeyCode::Escape;
    case VK_BACK: return KeyCode::Backspace;
    case VK_DELETE: return KeyCode::Delete;
    case VK_TAB: return KeyCode::Tab;
    case VK_INSERT: return KeyCode::Insert;
    case VK_OEM_PLUS:
    case VK_ADD: return KeyCode::Plus;
    case VK_OEM_MINUS:
    case VK_SUBTRACT: return KeyCode::Minus;
    default: return KeyCode::None;
    }
}

void MainWindow::onDropFiles(WPARAM wp) {
    HDROP drop = reinterpret_cast<HDROP>(wp);
    wchar_t path[MAX_PATH]{};
    if (DragQueryFileW(drop, 0, path, MAX_PATH) > 0 && app_) {
        app_->openPath(path);
    }
    DragFinish(drop);
}

void MainWindow::requestRedraw() {
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::setTitle(const std::wstring& title) {
    SetWindowTextW(hwnd_, title.c_str());
}

void MainWindow::setFullscreen(bool enabled) {
    if (enabled == fullscreen_) return;
    if (enabled) {
        savedStyle_ = GetWindowLongW(hwnd_, GWL_STYLE);
        GetWindowPlacement(hwnd_, &savedPlacement_);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongW(hwnd_, GWL_STYLE, savedStyle_ & ~WS_OVERLAPPEDWINDOW);
        SetWindowPos(hwnd_, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    } else {
        SetWindowLongW(hwnd_, GWL_STYLE, savedStyle_);
        SetWindowPlacement(hwnd_, &savedPlacement_);
        SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER |
                         SWP_FRAMECHANGED);
    }
    fullscreen_ = enabled;
}

std::optional<std::filesystem::path> MainWindow::showOpenDialog() {
    std::wstring patterns;
    for (const auto ext : kImageExtensions) {
        if (!patterns.empty()) patterns += L';';
        patterns += L'*';
        patterns += ext;
    }
    // OPENFILENAME のフィルタは NUL 区切り・二重 NUL 終端
    std::wstring filter;
    filter += L"画像ファイル";
    filter.push_back(L'\0');
    filter += patterns;
    filter.push_back(L'\0');
    filter += L"すべてのファイル";
    filter.push_back(L'\0');
    filter += L"*.*";
    filter.push_back(L'\0');

    wchar_t fileBuffer[MAX_PATH]{};
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = fileBuffer;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&ofn)) return std::nullopt;
    return std::filesystem::path(fileBuffer);
}

void MainWindow::quit() {
    DestroyWindow(hwnd_);
}

} // namespace blinker
