#include "win/window_win.h"

#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <vector>

#include "platform/image_formats.h"

namespace blinker {
namespace {

constexpr wchar_t kWindowClass[] = L"BlinkerMainWindow";
constexpr int kIconResourceId = 101;  // blinker.rc の IDI_APPICON
constexpr UINT_PTR kMessageTimerId = 1;
constexpr WORD kTextInputEditId = 1000;

// テキスト入力ダイアログの入出力。initial を初期表示し、OK なら result に書く
struct TextInputParams {
    const std::wstring* initial;
    std::wstring* result;
};

INT_PTR CALLBACK textInputDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_INITDIALOG: {
        SetWindowLongPtrW(dlg, DWLP_USER, lp);
        const auto* params = reinterpret_cast<const TextInputParams*>(lp);
        if (params->initial && !params->initial->empty()) {
            // 複数行エディットは CRLF 区切りのため '\n' を戻して設定する
            std::wstring text = *params->initial;
            for (size_t pos = 0; (pos = text.find(L'\n', pos)) != std::wstring::npos;
                 pos += 2) {
                text.replace(pos, 1, L"\r\n");
            }
            SetDlgItemTextW(dlg, kTextInputEditId, text.c_str());
        }
        return TRUE;  // 最初の WS_TABSTOP (エディット) にフォーカスさせる
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            auto* result =
                reinterpret_cast<TextInputParams*>(GetWindowLongPtrW(dlg, DWLP_USER))->result;
            const HWND edit = GetDlgItem(dlg, kTextInputEditId);
            const int length = GetWindowTextLengthW(edit);
            result->assign(static_cast<size_t>(std::max(length, 0)), L'\0');
            if (length > 0) GetWindowTextW(edit, result->data(), length + 1);
            // 複数行エディットの改行 CRLF を '\n' に正規化する
            for (size_t pos = 0; (pos = result->find(L"\r\n", pos)) != std::wstring::npos;) {
                result->replace(pos, 2, L"\n");
            }
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) {
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

// DialogBoxIndirectParam 用のメモリ内テンプレート。WORD 列で組み立てる
// (DLGTEMPLATE の後にメニュー・クラス・タイトル、各アイテムは DWORD 境界に揃える)
std::vector<WORD> buildTextInputTemplate() {
    std::vector<WORD> buf;
    const auto pushW = [&buf](WORD w) { buf.push_back(w); };
    const auto pushDW = [&buf](DWORD d) {
        buf.push_back(LOWORD(d));
        buf.push_back(HIWORD(d));
    };
    const auto pushStr = [&buf](const wchar_t* s) {
        for (; *s; ++s) buf.push_back(static_cast<WORD>(*s));
        buf.push_back(0);
    };
    const auto alignDword = [&buf] {
        if (buf.size() % 2) buf.push_back(0);
    };

    pushDW(DS_MODALFRAME | DS_SETFONT | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU);
    pushDW(0);   // 拡張スタイル
    pushW(3);    // アイテム数
    pushW(0);    // x
    pushW(0);    // y
    pushW(220);  // cx (ダイアログ単位)
    pushW(96);   // cy
    pushW(0);    // メニューなし
    pushW(0);    // 既定のダイアログクラス
    pushStr(L"テキスト (Enter で改行)");
    pushW(9);  // フォントサイズ (pt)
    pushStr(L"MS Shell Dlg");

    struct Item {
        DWORD style;
        WORD x, y, cx, cy, id, classAtom;
        const wchar_t* text;
    };
    const Item items[] = {
        {WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | WS_VSCROLL | ES_MULTILINE |
             ES_AUTOVSCROLL | ES_WANTRETURN,
         7, 7, 206, 60, kTextInputEditId, 0x0081, L""},  // EDIT (複数行)
        {WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 105, 74, 50, 14, IDOK, 0x0080,
         L"OK"},
        {WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 160, 74, 53, 14, IDCANCEL, 0x0080,
         L"キャンセル"},
    };
    for (const Item& item : items) {
        alignDword();
        pushDW(item.style);
        pushDW(0);  // 拡張スタイル
        pushW(item.x);
        pushW(item.y);
        pushW(item.cx);
        pushW(item.cy);
        pushW(item.id);
        pushW(0xFFFF);  // クラスはアトム指定
        pushW(item.classAtom);
        pushStr(item.text);
        pushW(0);  // 追加データなし
    }
    return buf;
}

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
    case WM_MOUSELEAVE:
        trackingMouseLeave_ = false;
        if (app_) app_->onMouseLeave();
        return 0;
    case WM_TIMER:
        if (wp == kMessageTimerId) {
            KillTimer(hwnd_, kMessageTimerId);  // 単発
            if (app_) app_->onTimer();
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

std::optional<std::filesystem::path> MainWindow::showSaveDialog(
    const std::wstring& defaultFileName) {
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
            AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sub), item.text.c_str());
            continue;
        }
        AppendMenuW(menu, MF_STRING | (item.checked ? MF_CHECKED : 0u), nextId++,
                    item.text.c_str());
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

std::optional<std::wstring> MainWindow::showTextInput(const std::wstring& initial) {
    const std::vector<WORD> dlgTemplate = buildTextInputTemplate();
    std::wstring result;
    TextInputParams params{&initial, &result};
    const INT_PTR code = DialogBoxIndirectParamW(
        reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd_, GWLP_HINSTANCE)),
        reinterpret_cast<const DLGTEMPLATE*>(dlgTemplate.data()), hwnd_, textInputDlgProc,
        reinterpret_cast<LPARAM>(&params));
    if (code != IDOK) return std::nullopt;
    return result;
}

void MainWindow::startTimer(unsigned milliseconds) {
    SetTimer(hwnd_, kMessageTimerId, milliseconds, nullptr);  // 既存タイマーは上書きされる
}

void MainWindow::quit() {
    DestroyWindow(hwnd_);
}

} // namespace blinker
