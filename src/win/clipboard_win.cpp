#include "win/clipboard_win.h"

#include <shlobj.h>  // DROPFILES / CFSTR_PREFERREDDROPEFFECT / DROPEFFECT_COPY

#include <algorithm>
#include <cstring>
#include <system_error>
#include <vector>

#include "core/dib.h"
#include "core/unicode.h"

namespace blinker {
namespace {

// data をコピーした GMEM_MOVEABLE メモリを返す。失敗時 nullptr
HGLOBAL allocGlobal(const void* data, size_t size) {
    HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!handle) return nullptr;
    void* dest = GlobalLock(handle);
    if (!dest) {
        GlobalFree(handle);
        return nullptr;
    }
    std::memcpy(dest, data, size);
    GlobalUnlock(handle);
    return handle;
}

// 他プロセスが掴んでいる瞬間に備えて少しだけリトライする
bool openClipboard(HWND owner) {
    for (int i = 0; i < 5; ++i) {
        if (OpenClipboard(owner)) return true;
        Sleep(10);
    }
    return false;
}

// CF_DIBV5: BITMAPV5HEADER + 32bpp BGRA(ストレートアルファ、ボトムアップ)
std::vector<uint8_t> buildDibV5(const DecodedImage& image) {
    const uint32_t w = image.width;
    const uint32_t h = image.height;
    std::vector<uint8_t> out(sizeof(BITMAPV5HEADER) + static_cast<size_t>(w) * h * 4);

    BITMAPV5HEADER header{};
    header.bV5Size = sizeof(BITMAPV5HEADER);
    header.bV5Width = static_cast<LONG>(w);
    header.bV5Height = static_cast<LONG>(h);  // 正 = ボトムアップ
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5SizeImage = w * h * 4;
    header.bV5RedMask = 0x00FF0000;
    header.bV5GreenMask = 0x0000FF00;
    header.bV5BlueMask = 0x000000FF;
    header.bV5AlphaMask = 0xFF000000;
    header.bV5CSType = LCS_sRGB;
    header.bV5Intent = LCS_GM_IMAGES;
    std::memcpy(out.data(), &header, sizeof(header));

    uint8_t* dst = out.data() + sizeof(header);
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* src = image.pixels.data() + static_cast<size_t>(h - 1 - y) * w * 4;
        uint8_t* row = dst + static_cast<size_t>(y) * w * 4;
        for (uint32_t x = 0; x < w; ++x) {
            const uint8_t a = src[x * 4 + 3];
            // 事前乗算 → ストレートアルファへ戻す(四捨五入)
            for (int c = 0; c < 3; ++c) {
                const uint8_t p = src[x * 4 + c];
                row[x * 4 + c] =
                    a == 0 ? 0
                           : static_cast<uint8_t>(std::min(255u, (p * 255u + a / 2u) / a));
            }
            row[x * 4 + 3] = a;
        }
    }
    return out;
}

// CF_DIB: BITMAPINFOHEADER + 24bpp BGR(白背景に合成、ボトムアップ、行4バイト境界)
std::vector<uint8_t> buildDib24(const DecodedImage& image) {
    const uint32_t w = image.width;
    const uint32_t h = image.height;
    const uint32_t stride = (w * 3 + 3) & ~3u;
    std::vector<uint8_t> out(sizeof(BITMAPINFOHEADER) + static_cast<size_t>(stride) * h);

    BITMAPINFOHEADER header{};
    header.biSize = sizeof(BITMAPINFOHEADER);
    header.biWidth = static_cast<LONG>(w);
    header.biHeight = static_cast<LONG>(h);
    header.biPlanes = 1;
    header.biBitCount = 24;
    header.biCompression = BI_RGB;
    header.biSizeImage = stride * h;
    std::memcpy(out.data(), &header, sizeof(header));

    uint8_t* dst = out.data() + sizeof(header);
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* src = image.pixels.data() + static_cast<size_t>(h - 1 - y) * w * 4;
        uint8_t* row = dst + static_cast<size_t>(y) * stride;
        for (uint32_t x = 0; x < w; ++x) {
            const uint8_t a = src[x * 4 + 3];
            // 事前乗算値に白の寄与 (255 - a) を足すだけで白背景合成になる
            for (int c = 0; c < 3; ++c) {
                row[x * 3 + c] = static_cast<uint8_t>(
                    std::min(255u, static_cast<uint32_t>(src[x * 4 + c]) + (255u - a)));
            }
        }
    }
    return out;
}

} // namespace

bool ClipboardWin::setImage(const DecodedImage& image) {
    if (image.width == 0 || image.height == 0) return false;

    // クリップボードを開く前にデータを作り終えて、保持時間を最小にする
    const std::vector<uint8_t> dibV5 = buildDibV5(image);
    const std::vector<uint8_t> dib24 = buildDib24(image);
    HGLOBAL handleV5 = allocGlobal(dibV5.data(), dibV5.size());
    HGLOBAL handle24 = allocGlobal(dib24.data(), dib24.size());
    if (!handleV5 || !handle24 || !openClipboard(owner_)) {
        if (handleV5) GlobalFree(handleV5);
        if (handle24) GlobalFree(handle24);
        return false;
    }

    EmptyClipboard();
    bool ok = true;
    // SetClipboardData 成功後の所有権はシステム側。失敗時のみ自分で解放する
    if (!SetClipboardData(CF_DIBV5, handleV5)) {
        GlobalFree(handleV5);
        ok = false;
    }
    if (!SetClipboardData(CF_DIB, handle24)) {
        GlobalFree(handle24);
        ok = false;
    }
    CloseClipboard();
    return ok;
}

bool ClipboardWin::setFiles(const std::vector<std::filesystem::path>& paths) {
    if (paths.empty()) return false;

    // CF_HDROP のパス列: 各パスを NUL 終端で並べ、末尾にもう 1 つ NUL を置く。
    // 貼り付け先のカレントディレクトリは当てにできないため必ず絶対パスにする
    std::wstring list;
    for (const auto& path : paths) {
        std::error_code ec;
        const std::filesystem::path full = std::filesystem::absolute(path, ec);
        list += (ec ? path : full).native();
        list.push_back(L'\0');
    }
    list.push_back(L'\0');

    // DROPFILES ヘッダの直後にパス列が続く 1 つのメモリブロック
    std::vector<uint8_t> buffer(sizeof(DROPFILES) + list.size() * sizeof(wchar_t));
    DROPFILES header{};
    header.pFiles = static_cast<DWORD>(sizeof(DROPFILES));  // パス列までのオフセット
    header.fWide = TRUE;
    std::memcpy(buffer.data(), &header, sizeof(header));
    std::memcpy(buffer.data() + sizeof(header), list.data(), list.size() * sizeof(wchar_t));

    // 貼り付け側が「移動」と解釈して元ファイルを消さないよう、コピーであることを明示する
    // CFSTR_PREFERREDDROPEFFECT は TCHAR マクロ。UNICODE 定義下なので W 版に解決される
    const UINT dropEffectFormat = RegisterClipboardFormat(CFSTR_PREFERREDDROPEFFECT);
    const DWORD dropEffect = DROPEFFECT_COPY;

    HGLOBAL handleFiles = allocGlobal(buffer.data(), buffer.size());
    HGLOBAL handleEffect = allocGlobal(&dropEffect, sizeof(dropEffect));
    if (!handleFiles || !handleEffect || dropEffectFormat == 0 || !openClipboard(owner_)) {
        if (handleFiles) GlobalFree(handleFiles);
        if (handleEffect) GlobalFree(handleEffect);
        return false;
    }

    EmptyClipboard();
    const bool ok = SetClipboardData(CF_HDROP, handleFiles) != nullptr;
    if (!ok) GlobalFree(handleFiles);
    // 効果の指定は失敗しても既定(コピー)で貼り付けられるため成否には含めない
    if (!SetClipboardData(dropEffectFormat, handleEffect)) GlobalFree(handleEffect);
    CloseClipboard();
    return ok;
}

std::shared_ptr<DecodedImage> ClipboardWin::getImage() {
    if (!openClipboard(owner_)) return nullptr;
    std::shared_ptr<DecodedImage> image;
    // CF_DIBV5(アルファ保持)を優先。どちらか一方しかないときは OS が相互に合成する
    for (const UINT format : {CF_DIBV5, CF_DIB}) {
        HANDLE handle = GetClipboardData(format);
        if (!handle) continue;
        if (const void* data = GlobalLock(static_cast<HGLOBAL>(handle))) {
            image = imageFromDib(static_cast<const uint8_t*>(data),
                                 GlobalSize(static_cast<HGLOBAL>(handle)));
            GlobalUnlock(static_cast<HGLOBAL>(handle));
        }
        if (image) break;
    }
    CloseClipboard();
    return image;
}

bool ClipboardWin::setText(const std::string& textUtf8) {
    const std::wstring text = utf8ToWide(textUtf8);
    HGLOBAL handle = allocGlobal(text.c_str(), (text.size() + 1) * sizeof(wchar_t));
    if (!handle || !openClipboard(owner_)) {
        if (handle) GlobalFree(handle);
        return false;
    }
    EmptyClipboard();
    const bool ok = SetClipboardData(CF_UNICODETEXT, handle) != nullptr;
    if (!ok) GlobalFree(handle);
    CloseClipboard();
    return ok;
}

std::string ClipboardWin::getText() {
    if (!openClipboard(owner_)) return {};
    std::wstring wide;
    if (HANDLE handle = GetClipboardData(CF_UNICODETEXT)) {
        if (const void* data = GlobalLock(static_cast<HGLOBAL>(handle))) {
            const auto* chars = static_cast<const wchar_t*>(data);
            // GlobalSize は要求より大きいことがあるため終端 NUL までを採る
            const size_t limit = GlobalSize(static_cast<HGLOBAL>(handle)) / sizeof(wchar_t);
            wide.assign(chars, std::find(chars, chars + limit, L'\0'));
            GlobalUnlock(static_cast<HGLOBAL>(handle));
        }
    }
    CloseClipboard();
    std::string text = wideToUtf8(wide);
    // 改行は注釈テキストの表現に合わせて LF に正規化する
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    return text;
}

} // namespace blinker
