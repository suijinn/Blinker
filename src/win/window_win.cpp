#include "win/window_win.h"

#include <commdlg.h>
#include <dwmapi.h>
#include <imm.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <vector>

#include "core/unicode.h"
#include "platform/image_formats.h"

namespace blinker {
namespace {

constexpr wchar_t kWindowClass[] = L"BlinkerMainWindow";
constexpr int kIconResourceId = 101;  // blinker.rc の IDI_APPICON
constexpr UINT_PTR kMessageTimerId = 1;
constexpr UINT_PTR kCaretTimerId = 2;

} // namespace

bool MainWindow::create(HINSTANCE hinstance, int showCommand, bool darkTitleBar) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;  // ダブルクリックでテキスト再編集
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

    // 通常時のキー入力はコマンドなので IME を切っておく(編集開始時に有効化する)
    ImmAssociateContextEx(hwnd_, nullptr, 0);

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
    case WM_CHAR:
        handleChar(static_cast<wchar_t>(wp));
        return 0;
    case WM_IME_STARTCOMPOSITION:
        updateImePosition();  // 変換開始のたびに位置を合わせ直す
        if (app_) app_->beginComposition();
        return 0;  // 既定の変換ウィンドウを出させない(変換中文字列は自前で描く)
    case WM_IME_COMPOSITION:
        // 確定文字列を先に取り込み、残った変換中文字列をインライン表示へ反映する
        handleImeResult(lp);
        handleImeComposition(lp);
        updateImePosition();  // キャレットが動くので候補ウィンドウを追従させる
        return 0;             // 既定の変換ウィンドウと WM_IME_CHAR を抑止する
    case WM_IME_ENDCOMPOSITION:
        if (app_) app_->clearComposition();
        return 0;
    case WM_IME_CHAR:
        return 0;  // 確定文字列は handleImeResult で挿入済み
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
        handleLeftDown({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        return 0;
    case WM_LBUTTONDBLCLK: {
        const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (app_ &&
            app_->onDoubleClick({static_cast<float>(pt.x), static_cast<float>(pt.y)})) {
            return 0;
        }
        handleLeftDown(pt);  // テキスト注釈以外の上では通常のクリックとして扱う
        return 0;
    }
    case WM_MOUSEMOVE: {
        const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (!trackingMouseLeave_) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
            trackingMouseLeave_ = TrackMouseEvent(&tme) != FALSE;
        }
        if (app_) {
            if (dragging_) {
                app_->onDragPan(static_cast<float>(pt.x - lastDragPos_.x),
                                static_cast<float>(pt.y - lastDragPos_.y));
                lastDragPos_ = pt;
            }
            if (rightDragging_) {
                app_->onRightDragMove({static_cast<float>(pt.x), static_cast<float>(pt.y)});
            }
            app_->onMouseMove({static_cast<float>(pt.x), static_cast<float>(pt.y)},
                              (GetKeyState(VK_SHIFT) & 0x8000) != 0);
        }
        return 0;
    }
    case WM_RBUTTONDOWN: {
        const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        rightDragging_ = true;
        SetCapture(hwnd_);
        if (app_) app_->onRightDragStart({static_cast<float>(pt.x), static_cast<float>(pt.y)});
        return 0;
    }
    case WM_RBUTTONUP: {
        if (!rightDragging_) break;
        rightDragging_ = false;
        ReleaseCapture();  // メニュー表示より先にキャプチャを解放する
        const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (app_) app_->onRightDragEnd({static_cast<float>(pt.x), static_cast<float>(pt.y)});
        return 0;
    }
    case WM_SETCURSOR:
        // 編集中のテキストボックスの内側だけ I ビームにする。
        // クライアント領域以外(枠・タイトルバー)は既定の処理に任せる
        if (LOWORD(lp) == HTCLIENT && app_) {
            POINT pt{};
            if (GetCursorPos(&pt) && ScreenToClient(hwnd_, &pt) &&
                app_->wantsTextCursor(
                    {static_cast<float>(pt.x), static_cast<float>(pt.y)})) {
                SetCursor(LoadCursorW(nullptr, IDC_IBEAM));
                return TRUE;
            }
        }
        break;
    case WM_MOUSELEAVE:
        trackingMouseLeave_ = false;
        if (app_) app_->onMouseLeave();
        return 0;
    case WM_TIMER:
        if (wp == kMessageTimerId) {
            KillTimer(hwnd_, kMessageTimerId);  // 単発
            if (app_) app_->onTimer();
        } else if (wp == kCaretTimerId) {
            if (app_) app_->onCaretBlink();  // 編集終了で KillTimer される
        }
        return 0;
    case WM_LBUTTONUP:
        dragging_ = false;
        if (GetCapture() == hwnd_) ReleaseCapture();
        if (app_) app_->onMouseUp();  // 注釈の移動・回転ドラッグを終了する
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

void MainWindow::handleLeftDown(POINT pt) {
    // サイドバー上のクリック(項目ジャンプ)や注釈のドラッグ開始ならパンを開始しない。
    // 注釈ドラッグ中の移動・ボタン解放を受けるため、消費されてもキャプチャは取る
    SetCapture(hwnd_);
    if (app_ && app_->onMouseDown({static_cast<float>(pt.x), static_cast<float>(pt.y)})) {
        return;
    }
    dragging_ = true;
    lastDragPos_ = pt;
}

void MainWindow::onPaint() {
    PAINTSTRUCT ps;
    BeginPaint(hwnd_, &ps);
    if (app_ && renderer_) {
        renderer_->render(app_->currentImage(), app_->imageToScreen(), app_->zoom(),
                          app_->backgroundRGB(), app_->annotations(), app_->selection(),
                          app_->sidebar(), app_->statusBar());
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

void MainWindow::setTitle(const std::string& title) {
    SetWindowTextW(hwnd_, utf8ToWide(title).c_str());
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
        patterns += utf8ToWide(ext);  // 拡張子は ASCII
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

std::optional<std::filesystem::path> MainWindow::showSaveDialog(
    const std::string& defaultFileNameUtf8) {
    const std::wstring defaultFileName = utf8ToWide(defaultFileNameUtf8);
    // フィルタ順は EncoderWic の対応形式と一致させる (1=PNG, 2=JPEG, 3=BMP)
    std::wstring filter;
    filter += L"PNG (*.png)";
    filter.push_back(L'\0');
    filter += L"*.png";
    filter.push_back(L'\0');
    filter += L"JPEG (*.jpg;*.jpeg)";
    filter.push_back(L'\0');
    filter += L"*.jpg;*.jpeg";
    filter.push_back(L'\0');
    filter += L"BMP (*.bmp)";
    filter.push_back(L'\0');
    filter += L"*.bmp";
    filter.push_back(L'\0');

    wchar_t fileBuffer[MAX_PATH]{};
    const size_t copyLength = std::min(defaultFileName.size(), static_cast<size_t>(MAX_PATH - 1));
    defaultFileName.copy(fileBuffer, copyLength);

    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = fileBuffer;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_HIDEREADONLY | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&ofn)) return std::nullopt;

    // 拡張子なしで入力されたら選択中のフィルタに合わせて補う
    // (lpstrDefExt では PNG 固定になってしまうため自前で行う)
    std::filesystem::path result(fileBuffer);
    if (!result.has_extension()) {
        result += ofn.nFilterIndex == 2 ? L".jpg" : ofn.nFilterIndex == 3 ? L".bmp" : L".png";
    }
    return result;
}

namespace {

// MenuItem 木を HMENU に組み立てる。末端項目に深さ優先で 1 始まりの ID を振る
// (ID 0 はキャンセルと区別できないため)。サブメニューは親メニューの破棄で一緒に破棄される
bool appendMenuItems(HMENU menu, const std::vector<MenuItem>& items, UINT& nextId) {
    for (const MenuItem& item : items) {
        if (item.separator) {
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            continue;
        }
        if (!item.children.empty()) {
            HMENU sub = CreatePopupMenu();
            if (!sub) return false;
            if (!appendMenuItems(sub, item.children, nextId)) {
                DestroyMenu(sub);
                return false;
            }
            AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sub),
                        utf8ToWide(item.text).c_str());
            continue;
        }
        AppendMenuW(menu, MF_STRING | (item.checked ? MF_CHECKED : 0u), nextId++,
                    utf8ToWide(item.text).c_str());
    }
    return true;
}

} // namespace

std::optional<size_t> MainWindow::showContextMenu(const std::vector<MenuItem>& items,
                                                  Point screenPos) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return std::nullopt;
    UINT nextId = 1;
    if (!appendMenuItems(menu, items, nextId)) {
        DestroyMenu(menu);
        return std::nullopt;
    }
    POINT pt{static_cast<LONG>(screenPos.x), static_cast<LONG>(screenPos.y)};
    ClientToScreen(hwnd_, &pt);
    const int selected = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                                        pt.x, pt.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);
    if (selected <= 0) return std::nullopt;
    return static_cast<size_t>(selected - 1);
}

std::optional<uint32_t> MainWindow::showColorPicker(uint32_t initialRGB) {
    static COLORREF customColors[16]{};  // ダイアログの「作成した色」をセッション内で保持
    CHOOSECOLORW cc{sizeof(cc)};
    cc.hwndOwner = hwnd_;
    cc.rgbResult = RGB((initialRGB >> 16) & 0xFF, (initialRGB >> 8) & 0xFF, initialRGB & 0xFF);
    cc.lpCustColors = customColors;
    cc.Flags = CC_RGBINIT | CC_FULLOPEN;
    if (!ChooseColorW(&cc)) return std::nullopt;
    return (static_cast<uint32_t>(GetRValue(cc.rgbResult)) << 16) |
           (static_cast<uint32_t>(GetGValue(cc.rgbResult)) << 8) |
           static_cast<uint32_t>(GetBValue(cc.rgbResult));
}

void MainWindow::setTextEditing(bool active, Point caretScreenPos, float caretHeightPx) {
    caretPos_ = {static_cast<LONG>(caretScreenPos.x), static_cast<LONG>(caretScreenPos.y)};
    caretHeight_ = static_cast<int>(caretHeightPx);
    if (active == textEditing_) {
        if (active) updateImePosition();  // キャレット移動だけの通知
        return;
    }
    textEditing_ = active;
    if (active) {
        // 通常時は 'n' 等がコマンドのため IME を切っている。編集中だけ有効化する
        ImmAssociateContextEx(hwnd_, nullptr, IACE_DEFAULT);
        updateImePosition();
        SetTimer(hwnd_, kCaretTimerId, GetCaretBlinkTime(), nullptr);
    } else {
        KillTimer(hwnd_, kCaretTimerId);
        pendingSurrogate_ = 0;
        if (HIMC imc = ImmGetContext(hwnd_)) {
            ImmNotifyIME(imc, NI_COMPOSITIONSTR, CPS_CANCEL, 0);  // 変換中なら捨てる
            ImmReleaseContext(hwnd_, imc);
        }
        ImmAssociateContextEx(hwnd_, nullptr, 0);
    }
    refreshCursor();
}

void MainWindow::refreshCursor() {
    // 編集の開始・終了ではマウスが動かず WM_SETCURSOR が来ないため自分で送る
    // (I ビームのまま残る/矢印のままになるのを防ぐ)。編集外なら DefWindowProc が
    // クラスの矢印カーソルへ戻す
    SendMessageW(hwnd_, WM_SETCURSOR, reinterpret_cast<WPARAM>(hwnd_),
                 MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
}

void MainWindow::updateImePosition() {
    HIMC imc = ImmGetContext(hwnd_);
    if (!imc) return;
    // 変換中文字列は自前でテキストボックス内に描くため、変換ウィンドウは使わない。
    // 候補ウィンドウをキャレットの真下に出すために位置だけ知らせる(クライアント座標)
    COMPOSITIONFORM form{};
    form.dwStyle = CFS_POINT;
    form.ptCurrentPos = {caretPos_.x, caretPos_.y};
    ImmSetCompositionWindow(imc, &form);
    CANDIDATEFORM candidate{};
    candidate.dwStyle = CFS_EXCLUDE;  // キャレットの行に重ならない位置へ出させる
    candidate.ptCurrentPos = {caretPos_.x, caretPos_.y};
    candidate.rcArea = {caretPos_.x, caretPos_.y, caretPos_.x + 1,
                        caretPos_.y + std::max(caretHeight_, 1)};
    ImmSetCandidateWindow(imc, &candidate);
    ImmReleaseContext(hwnd_, imc);
}

// 変換中文字列(GCS_COMPSTR)を読み、App へインライン表示させる
void MainWindow::handleImeComposition(LPARAM lp) {
    if (!app_ || !app_->isTextEditing()) return;
    HIMC imc = ImmGetContext(hwnd_);
    if (!imc) return;
    const LONG bytes = ImmGetCompositionStringW(imc, GCS_COMPSTR, nullptr, 0);
    if (bytes <= 0) {
        app_->clearComposition();
        ImmReleaseContext(hwnd_, imc);
        return;
    }
    const size_t units = static_cast<size_t>(bytes) / sizeof(wchar_t);
    std::wstring wide(units, L'\0');
    ImmGetCompositionStringW(imc, GCS_COMPSTR, wide.data(), static_cast<DWORD>(bytes));

    // 属性は UTF-16 コード単位ごとに 1 バイト。変換対象の節は連続した範囲になる
    size_t targetBeginUnits = 0;
    size_t targetEndUnits = 0;
    if (lp & GCS_COMPATTR) {
        const LONG attrBytes = ImmGetCompositionStringW(imc, GCS_COMPATTR, nullptr, 0);
        if (attrBytes > 0) {
            std::vector<BYTE> attrs(static_cast<size_t>(attrBytes));
            ImmGetCompositionStringW(imc, GCS_COMPATTR, attrs.data(),
                                     static_cast<DWORD>(attrBytes));
            const auto isTarget = [](BYTE a) {
                return a == ATTR_TARGET_CONVERTED || a == ATTR_TARGET_NOTCONVERTED;
            };
            const auto begin = std::find_if(attrs.begin(), attrs.end(), isTarget);
            if (begin != attrs.end()) {
                const auto end = std::find_if_not(begin, attrs.end(), isTarget);
                targetBeginUnits = static_cast<size_t>(begin - attrs.begin());
                targetEndUnits = static_cast<size_t>(end - attrs.begin());
            }
        }
    }
    // キャレット位置は UTF-16 コード単位で返る
    const LONG caretUnits = ImmGetCompositionStringW(imc, GCS_CURSORPOS, nullptr, 0);
    ImmReleaseContext(hwnd_, imc);

    // core 側はすべて UTF-8 のバイト位置で扱うため、ここで変換して渡す
    const std::string utf8 = wideToUtf8(wide);
    app_->setComposition(
        utf8, utf16ToUtf8Offset(utf8, caretUnits > 0 ? static_cast<size_t>(caretUnits) : 0),
        utf16ToUtf8Offset(utf8, targetBeginUnits), utf16ToUtf8Offset(utf8, targetEndUnits));
}

void MainWindow::handleChar(wchar_t ch) {
    if (!app_ || !app_->isTextEditing()) return;
    if (IS_HIGH_SURROGATE(ch)) {
        pendingSurrogate_ = ch;
        return;
    }
    std::wstring text;
    if (IS_LOW_SURROGATE(ch) && pendingSurrogate_ != 0) {
        text = {pendingSurrogate_, ch};
    } else if (ch >= 0x20) {
        // 制御文字 (Enter・Tab・Backspace・Esc) は WM_KEYDOWN 側で処理済み。
        // TranslateMessage は wndProc の戻り値に関係なく WM_CHAR を作るため、
        // ここで捨てないと二重に入力される
        text = {ch};
    }
    pendingSurrogate_ = 0;
    if (!text.empty()) app_->insertText(wideToUtf8(text));
}

void MainWindow::handleImeResult(LPARAM lp) {
    if (!app_ || !app_->isTextEditing() || !(lp & GCS_RESULTSTR)) return;
    HIMC imc = ImmGetContext(hwnd_);
    if (!imc) return;
    const LONG bytes = ImmGetCompositionStringW(imc, GCS_RESULTSTR, nullptr, 0);
    if (bytes > 0) {
        std::wstring text(static_cast<size_t>(bytes) / sizeof(wchar_t), L'\0');
        ImmGetCompositionStringW(imc, GCS_RESULTSTR, text.data(), static_cast<DWORD>(bytes));
        app_->insertText(wideToUtf8(text));
    }
    ImmReleaseContext(hwnd_, imc);
}

void MainWindow::startTimer(unsigned milliseconds) {
    SetTimer(hwnd_, kMessageTimerId, milliseconds, nullptr);  // 既存タイマーは上書きされる
}

void MainWindow::quit() {
    DestroyWindow(hwnd_);
}

} // namespace blinker
