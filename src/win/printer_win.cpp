#include "win/printer_win.h"

#include <windows.h>

#include <commdlg.h>

#include <algorithm>
#include <cmath>
#include <memory>

#include "core/exif.h"
#include "core/image_scale.h"
#include "core/print_layout.h"
#include "core/unicode.h"
#include "win/print_winrt.h"

namespace blinker {
namespace {

// mm をそのデバイスでのピクセル数へ直す
int millimetersToPixels(const float mm, const int dpi) {
    return static_cast<int>(std::lround(static_cast<double>(mm) * dpi / 25.4));
}

// 32bpp トップダウン DIB としてのヘッダ。GDI は上位バイトを無視するので、
// 事前乗算のアルファは(不透明な画像を渡す限り)問題にならない
BITMAPINFO dibHeaderFor(const DecodedImage& image) {
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = static_cast<LONG>(image.width);
    info.bmiHeader.biHeight = -static_cast<LONG>(image.height);  // 負 = 上から下へ
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    return info;
}

// PrintDlg が確保したものを経路によらず解放する
struct PrintDialogResult {
    HDC dc = nullptr;
    HGLOBAL devMode = nullptr;
    HGLOBAL devNames = nullptr;

    ~PrintDialogResult() {
        if (dc) DeleteDC(dc);
        if (devMode) GlobalFree(devMode);
        if (devNames) GlobalFree(devNames);
    }
};

} // namespace

PrintStatus PrinterWin::print(const DecodedImage& image, const std::string& jobName,
                              const PrintOptions& options) {
    if (image.width == 0 || image.height == 0) return PrintStatus::Failed;

    const std::wstring name = utf8ToWide(jobName);
    // まず OS のモダン印刷ダイアログ(プレビュー付き)を試す。使えない環境では
    // 従来の印刷ダイアログ (PrintDlg) へ落とす ―― そちらにプレビューは無い
    switch (printWithModernUi(owner_, image, name, options)) {
    case ModernPrintStatus::Printed:
        return PrintStatus::Printed;
    case ModernPrintStatus::Canceled:
        return PrintStatus::Canceled;
    case ModernPrintStatus::Failed:
        return PrintStatus::Failed;
    case ModernPrintStatus::Unavailable:
        break;
    }

    PRINTDLGW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = owner_;
    // 1 ページしか刷らないのでページ範囲・選択範囲は出さない。部数はダイアログ側に
    // 任せず自分で受け取り(PD_USEDEVMODECOPIESANDCOLLATE を付けない)、
    // 同じページを繰り返して刷る。ドライバが部数に対応していなくても効く
    dialog.Flags = PD_RETURNDC | PD_NOPAGENUMS | PD_NOSELECTION;
    dialog.nCopies = 1;
    if (!PrintDlgW(&dialog)) {
        // 取りやめと失敗は戻り値では区別できない(どちらも FALSE)
        return CommDlgExtendedError() == 0 ? PrintStatus::Canceled : PrintStatus::Failed;
    }
    const PrintDialogResult result{dialog.hDC, dialog.hDevMode, dialog.hDevNames};
    if (!result.dc) return PrintStatus::Failed;

    // プリンタ DC の原点は印刷可能領域の左上なので、物理オフセットの補正は要らない
    const int pageWidth = GetDeviceCaps(result.dc, HORZRES);
    const int pageHeight = GetDeviceCaps(result.dc, VERTRES);
    // 余白は印刷可能領域の 1/4 まで(大きすぎる指定で画像が消えないように)
    const int marginX = std::clamp(
        millimetersToPixels(options.marginMm, GetDeviceCaps(result.dc, LOGPIXELSX)), 0,
        pageWidth / 4);
    const int marginY = std::clamp(
        millimetersToPixels(options.marginMm, GetDeviceCaps(result.dc, LOGPIXELSY)), 0,
        pageHeight / 4);
    const PrintPlacement placement = layoutPrintImage(
        static_cast<int>(image.width), static_cast<int>(image.height), pageWidth - marginX * 2,
        pageHeight - marginY * 2, options.autoRotate);
    if (placement.width <= 0 || placement.height <= 0) return PrintStatus::Failed;

    // 印刷される大きさを超える画素はドライバへ渡しても意味がない。箱型フィルタで
    // 先に縮めておくと GDI の間引きより結果がきれいで、転送量も減る
    std::shared_ptr<DecodedImage> prepared;
    const DecodedImage* source = &image;
    const auto printedMax = static_cast<uint32_t>(std::max(placement.width, placement.height));
    if (std::max(image.width, image.height) > printedMax) {
        if (auto reduced = downscaleToFit(image, printedMax)) {
            prepared = std::move(reduced);
            source = prepared.get();
        }
    }
    if (placement.rotated) {
        // EXIF Orientation の 6 が時計回り 90 度(App の表示回転の焼き込みと同じ流用)
        if (!prepared) prepared = std::make_shared<DecodedImage>(*source);
        if (!applyExifOrientation(*prepared, 6)) return PrintStatus::Failed;
        source = prepared.get();
    }

    DOCINFOW docInfo{};
    docInfo.cbSize = sizeof(docInfo);
    docInfo.lpszDocName = name.empty() ? L"Blinker" : name.c_str();
    // ダイアログの「ファイルへ出力」。指定しないとチェックしても普通に印刷されてしまう
    // (出力先のファイル名は GDI が尋ねる)
    if (dialog.Flags & PD_PRINTTOFILE) docInfo.lpszOutput = L"FILE:";
    if (StartDocW(result.dc, &docInfo) <= 0) return PrintStatus::Failed;

    const BITMAPINFO info = dibHeaderFor(*source);
    const int copies = std::clamp(static_cast<int>(dialog.nCopies), 1, 999);
    bool ok = true;
    for (int copy = 0; ok && copy < copies; ++copy) {
        if (StartPage(result.dc) <= 0) {
            ok = false;
            break;
        }
        // DC の属性はページごとに戻りうるので、StartPage の後に設定する
        SetStretchBltMode(result.dc, HALFTONE);
        SetBrushOrgEx(result.dc, 0, 0, nullptr);  // HALFTONE の要求
        const int copied = StretchDIBits(
            result.dc, marginX + placement.x, marginY + placement.y, placement.width,
            placement.height, 0, 0, static_cast<int>(source->width),
            static_cast<int>(source->height), source->pixels.data(), &info, DIB_RGB_COLORS,
            SRCCOPY);
        if (copied == static_cast<int>(GDI_ERROR)) ok = false;
        if (EndPage(result.dc) <= 0) ok = false;
    }
    if (!ok) {
        AbortDoc(result.dc);
        return PrintStatus::Failed;
    }
    return EndDoc(result.dc) > 0 ? PrintStatus::Printed : PrintStatus::Failed;
}

} // namespace blinker
