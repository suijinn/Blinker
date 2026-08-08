#include "win/print_winrt.h"

#include <windows.h>

#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wincodec.h>
#include <wrl/client.h>

// WinRT は ABI で直接呼ぶ(下ごしらえは win/winrt_abi.h)
#include <windows.foundation.h>
#include <windows.graphics.printing.h>

// 印刷 UI とドキュメントソースの COM インターフェース
#include <DocumentSource.h>
#include <DocumentTarget.h>
#include <PrintManagerInterop.h>
#include <PrintPreview.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <tuple>
#include <type_traits>

#include "core/image_scale.h"
#include "core/print_layout.h"
#include "win/wic_factory.h"
#include "win/winrt_abi.h"

namespace blinker {
namespace {

using Microsoft::WRL::ComPtr;

namespace wf = ABI::Windows::Foundation;
namespace wgp = ABI::Windows::Graphics::Printing;

using PrintTaskRequestedHandler =
    wf::ITypedEventHandler<wgp::PrintManager*, wgp::PrintTaskRequestedEventArgs*>;
using PrintTaskCompletedHandler =
    wf::ITypedEventHandler<wgp::PrintTask*, wgp::PrintTaskCompletedEventArgs*>;
using ShowPrintUiOperation = wf::IAsyncOperation<bool>;
using ShowPrintUiHandler = wf::IAsyncOperationCompletedHandler<bool>;

/// 印刷 UI での操作を待つ上限。完了通知が来ないまま放置された場合の保険
constexpr DWORD kUiTimeoutMs = 10 * 60 * 1000;
/// プレビュー 1 ページを描くサーフェスの一辺の上限・下限(画面より細かく持っても見えない)
constexpr UINT32 kPreviewMaxPixels = 2048;
constexpr UINT32 kPreviewMinPixels = 256;

/**
 * IUnknown / IInspectable の実装を配る基底。
 *
 * WRL の RuntimeClass を使わないのは、WinRT インターフェース (IInspectable) の実装に
 * 使う InspectableClass マクロが windowsapp.lib への静的リンクを要求するため。
 * このプロジェクトは combase を遅延解決に留めている(winrt_abi.h)。
 */
template <typename... Interfaces>
class ComObject : public Interfaces... {
public:
    virtual ~ComObject() = default;

    ULONG STDMETHODCALLTYPE AddRef() noexcept override { return ++refCount_; }

    ULONG STDMETHODCALLTYPE Release() noexcept override {
        const ULONG remaining = --refCount_;
        if (remaining == 0) delete this;
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) noexcept override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == __uuidof(IUnknown)) {
            using First = std::tuple_element_t<0, std::tuple<Interfaces...>>;
            *ppv = static_cast<IUnknown*>(static_cast<First*>(this));
            AddRef();
            return S_OK;
        }
        if (riid == __uuidof(IInspectable) && (castInspectable<Interfaces>(ppv) || ...)) {
            return S_OK;
        }
        if ((cast<Interfaces>(riid, ppv) || ...)) return S_OK;
        return E_NOINTERFACE;
    }

    // 以下は IInspectable を持つ派生でのみ実際の override になる(持たなければただの関数)。
    // 印刷 UI はクラス名しか聞いてこないので、他は最小限の実装でよい
    HRESULT STDMETHODCALLTYPE GetIids(ULONG* count, IID** iids) noexcept {
        if (!count || !iids) return E_POINTER;
        *count = 0;
        *iids = nullptr;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING* name) noexcept {
        if (!name) return E_POINTER;
        *name = nullptr;
        if (!comBase().ok) return E_NOTIMPL;
        constexpr wchar_t kName[] = L"Blinker.PrintDocumentSource";
        return comBase().windowsCreateString(kName, static_cast<UINT32>(std::size(kName) - 1),
                                             name);
    }

    HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel* level) noexcept {
        if (!level) return E_POINTER;
        *level = BaseTrust;
        return S_OK;
    }

private:
    template <typename I>
    bool cast(REFIID riid, void** ppv) noexcept {
        if (riid != __uuidof(I)) return false;
        *ppv = static_cast<I*>(this);
        AddRef();
        return true;
    }

    template <typename I>
    bool castInspectable(void** ppv) noexcept {
        if constexpr (std::is_base_of_v<IInspectable, I>) {
            *ppv = static_cast<IInspectable*>(static_cast<I*>(this));
            AddRef();
            return true;
        } else {
            (void)ppv;
            return false;
        }
    }

    std::atomic<ULONG> refCount_{1};
};

/**
 * 印刷とプレビューの描画に使う Direct2D 1.1 のデバイス一式。
 *
 * 画面描画の RendererD2D は D2D 1.0 の ID2D1HwndRenderTarget なので流用できない
 * (印刷とプレビューは D2D 1.1 のデバイス・コマンドリスト・印刷コントロールを要求する)。
 * 印刷のときだけ作り、終われば捨てる。
 */
struct PrintDevice {
    ComPtr<ID3D11Device> d3d;      ///< プレビュー用のテクスチャを作るのに要る
    ComPtr<ID2D1Factory1> factory;
    ComPtr<ID2D1Device> device;
};

std::shared_ptr<PrintDevice> createPrintDevice() {
    // d3d11.dll は印刷のときだけ読む(起動時のインポートを増やさない)
    static const auto createD3dDevice = [] {
        const HMODULE module =
            LoadLibraryExW(L"d3d11.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return module ? reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(
                            GetProcAddress(module, "D3D11CreateDevice"))
                      : nullptr;
    }();
    if (!createD3dDevice) return nullptr;

    auto result = std::make_shared<PrintDevice>();
    // D2D と共有するので BGRA サポートは必須。GPU が使えない環境では WARP へ落とす
    // (SINGLETHREADED は付けない ―― プレビューと本出力は OS 側のスレッドから来る)
    for (const D3D_DRIVER_TYPE type : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP}) {
        if (SUCCEEDED(createD3dDevice(nullptr, type, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                      nullptr, 0, D3D11_SDK_VERSION, &result->d3d, nullptr,
                                      nullptr))) {
            break;
        }
        result->d3d.Reset();
    }
    if (!result->d3d) return nullptr;

    ComPtr<IDXGIDevice> dxgi;
    if (FAILED(result->d3d.As(&dxgi))) return nullptr;
    D2D1_FACTORY_OPTIONS factoryOptions{};
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, __uuidof(ID2D1Factory1),
                                 &factoryOptions,
                                 reinterpret_cast<void**>(result->factory.GetAddressOf())))) {
        return nullptr;
    }
    if (FAILED(result->factory->CreateDevice(dxgi.Get(), &result->device))) return nullptr;
    return result;
}

/// 用紙 1 枚の上での画像の置き場所。単位は DIP (1/96 インチ) = D2D の座標系
struct PageLayout {
    float pageWidth = 0;       ///< 用紙全体の幅
    float pageHeight = 0;      ///< 用紙全体の高さ
    D2D1_RECT_F imageRect{};   ///< 画像を占める矩形(回転後の見た目の大きさ)
    bool rotated = false;      ///< 画像を時計回りに 90 度回して描くか
    bool valid = false;        ///< false なら配置できなかった
};

/**
 * 用紙の情報と設定から配置を決める。
 *
 * PrintPageDescription の単位はプリンタのデバイスピクセル (DpiX / DpiY) なので、
 * まず DIP へ直してから core の layoutPrintImage(GDI 経路と同じ計算)に渡す。
 * 整数で受ける関数なので 1/100 DIP まで持ち上げて渡し、戻ってきた値を割り戻す。
 */
PageLayout computePageLayout(const wgp::PrintPageDescription& page, const uint32_t imageWidth,
                             const uint32_t imageHeight, const PrintOptions& options) {
    PageLayout layout;
    const float dpiX = page.DpiX > 0 ? static_cast<float>(page.DpiX) : 96.0f;
    const float dpiY = page.DpiY > 0 ? static_cast<float>(page.DpiY) : 96.0f;
    const auto toDipX = [dpiX](const float pixels) { return pixels * 96.0f / dpiX; };
    const auto toDipY = [dpiY](const float pixels) { return pixels * 96.0f / dpiY; };

    layout.pageWidth = toDipX(page.PageSize.Width);
    layout.pageHeight = toDipY(page.PageSize.Height);
    if (!(layout.pageWidth > 0) || !(layout.pageHeight > 0)) return layout;

    // 印刷可能領域(用紙の端の、プリンタが物理的に刷れない縁を除いた範囲)
    float left = toDipX(page.ImageableRect.X);
    float top = toDipY(page.ImageableRect.Y);
    float width = toDipX(page.ImageableRect.Width);
    float height = toDipY(page.ImageableRect.Height);
    if (!(width > 0) || !(height > 0)) {
        left = 0;
        top = 0;
        width = layout.pageWidth;
        height = layout.pageHeight;
    }

    // 設定の余白はそこからさらに内側へ。印刷可能領域の 1/4 までに抑える
    // (大きすぎる指定で画像が消えないように。GDI 経路と同じ制限)
    const float margin = options.marginMm * 96.0f / 25.4f;
    const float marginX = std::clamp(margin, 0.0f, width / 4);
    const float marginY = std::clamp(margin, 0.0f, height / 4);

    constexpr float kScale = 100.0f;  // layoutPrintImage へ渡す 1/100 DIP 単位
    const PrintPlacement placement = layoutPrintImage(
        static_cast<int>(imageWidth), static_cast<int>(imageHeight),
        static_cast<int>(std::lround((width - marginX * 2) * kScale)),
        static_cast<int>(std::lround((height - marginY * 2) * kScale)), options.autoRotate);
    if (placement.width <= 0 || placement.height <= 0) return layout;

    const float x = left + marginX + placement.x / kScale;
    const float y = top + marginY + placement.y / kScale;
    layout.imageRect =
        D2D1::RectF(x, y, x + placement.width / kScale, y + placement.height / kScale);
    layout.rotated = placement.rotated;
    layout.valid = true;
    return layout;
}

/// 画像を D2D のビットマップにする。描画側の上限を超える画像は縮めてから載せる
ComPtr<ID2D1Bitmap> createImageBitmap(ID2D1DeviceContext* context, const DecodedImage& image) {
    const DecodedImage* source = &image;
    std::shared_ptr<DecodedImage> reduced;
    const UINT32 maximum = context->GetMaximumBitmapSize();
    if (std::max(image.width, image.height) > maximum) {
        reduced = downscaleToFit(image, maximum);
        if (!reduced) return nullptr;
        source = reduced.get();
    }
    const D2D1_BITMAP_PROPERTIES properties = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    ComPtr<ID2D1Bitmap> bitmap;
    if (FAILED(context->CreateBitmap(D2D1::SizeU(source->width, source->height),
                                     source->pixels.data(), source->width * 4, properties,
                                     &bitmap))) {
        return nullptr;
    }
    return bitmap;
}

/**
 * 用紙 1 枚を描く。呼び出し側が描画先の設定と BeginDraw / EndDraw を済ませていること。
 *
 * @param base 用紙の DIP 座標を描画先へ移す変換。本出力は等倍(単位行列)、
 *             プレビューは枠の大きさへ縮める倍率が入る。
 */
void drawPage(ID2D1DeviceContext* context, ID2D1Bitmap* bitmap, const PageLayout& layout,
              const D2D1_MATRIX_3X2_F& base) {
    context->SetTransform(D2D1::Matrix3x2F::Identity());
    context->Clear(D2D1::ColorF(D2D1::ColorF::White));  // 紙は白
    if (!bitmap) return;
    if (!layout.rotated) {
        context->SetTransform(base);
        context->DrawBitmap(bitmap, layout.imageRect, 1.0f,
                            D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
    } else {
        // 90 度回して刷る場合。imageRect は回転後の見た目の矩形なので、中心を保ったまま
        // 幅と高さを入れ替えた矩形へ描き、その中心まわりに回す
        const D2D1_POINT_2F center{(layout.imageRect.left + layout.imageRect.right) / 2,
                                   (layout.imageRect.top + layout.imageRect.bottom) / 2};
        const float halfWidth = (layout.imageRect.bottom - layout.imageRect.top) / 2;
        const float halfHeight = (layout.imageRect.right - layout.imageRect.left) / 2;
        context->SetTransform(D2D1::Matrix3x2F::Rotation(90.0f, center) * base);
        context->DrawBitmap(bitmap,
                            D2D1::RectF(center.x - halfWidth, center.y - halfHeight,
                                        center.x + halfWidth, center.y + halfHeight),
                            1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
    }
    context->SetTransform(D2D1::Matrix3x2F::Identity());
}

/// 印刷 UI と OS からのコールバックの進み具合。UI スレッドと OS のスレッドの両方が触る
struct PrintProgress {
    std::atomic<bool> uiDone{false};           ///< ShowPrintUIForWindowAsync が完了した
    std::atomic<bool> uiSucceeded{false};      ///< その完了が成功だったか
    std::atomic<bool> taskCreated{false};      ///< 印刷タスクを作った = ダイアログが出た
    std::atomic<bool> documentRequested{false};  ///< MakeDocument が来た = 「印刷」を押した
    std::atomic<int> completion{-1};           ///< PrintTaskCompletion。-1 は未完了
};

/// プレビューのページ(常に 1 ページ)。OS はこれを通じてページを要求してくる
class PreviewPages final : public ComObject<IPrintPreviewPageCollection> {
public:
    PreviewPages(std::shared_ptr<const DecodedImage> image, const PrintOptions& options,
                 std::shared_ptr<PrintDevice> device,
                 ComPtr<IPrintPreviewDxgiPackageTarget> target)
        : image_(std::move(image)),
          options_(options),
          device_(std::move(device)),
          target_(std::move(target)) {}

    HRESULT STDMETHODCALLTYPE Paginate(UINT32 currentJobPage,
                                       IInspectable* printTaskOptions) noexcept override {
        (void)currentJobPage;
        wgp::PrintPageDescription description{};
        const HRESULT hr = pageDescriptionOf(printTaskOptions, description);
        if (FAILED(hr)) return hr;
        {
            std::lock_guard lock(mutex_);
            description_ = description;
            hasDescription_ = true;
        }
        // 画像 1 枚は常に 1 ページ(タイル印刷はしない)
        return target_->SetJobPageCount(FinalPageCount, 1);
    }

    HRESULT STDMETHODCALLTYPE MakePage(UINT32 desiredJobPage, FLOAT width,
                                       FLOAT height) noexcept override {
        wgp::PrintPageDescription description{};
        {
            std::lock_guard lock(mutex_);
            if (!hasDescription_) return E_FAIL;  // Paginate より先には来ない
            description = description_;
        }
        const PageLayout layout =
            computePageLayout(description, image_->width, image_->height, options_);
        if (!layout.valid) return E_FAIL;
        if (!(width > 1.0f) || !(height > 1.0f)) return E_INVALIDARG;

        // プレビュー枠は 96dpi で合成される。つまり要求された大きさ(DIP)が
        // そのままサーフェスのピクセル数で、用紙はそこへ収まるよう縮めて描く
        const UINT32 pixelWidth =
            std::clamp(static_cast<UINT32>(std::lround(width)), kPreviewMinPixels,
                       kPreviewMaxPixels);
        const UINT32 pixelHeight =
            std::clamp(static_cast<UINT32>(std::lround(height)), kPreviewMinPixels,
                       kPreviewMaxPixels);
        const float scale =
            std::min(pixelWidth / layout.pageWidth, pixelHeight / layout.pageHeight);

        ComPtr<ID2D1DeviceContext> context;
        if (FAILED(device_->device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
                                                        &context))) {
            return E_FAIL;
        }
        D3D11_TEXTURE2D_DESC textureDesc{};
        textureDesc.Width = pixelWidth;
        textureDesc.Height = pixelHeight;
        textureDesc.MipLevels = 1;
        textureDesc.ArraySize = 1;
        textureDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        textureDesc.SampleDesc.Count = 1;
        textureDesc.Usage = D3D11_USAGE_DEFAULT;
        textureDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> texture;
        if (FAILED(device_->d3d->CreateTexture2D(&textureDesc, nullptr, &texture))) return E_FAIL;
        ComPtr<IDXGISurface> surface;
        if (FAILED(texture.As(&surface))) return E_FAIL;

        const D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        ComPtr<ID2D1Bitmap1> pageBitmap;
        if (FAILED(context->CreateBitmapFromDxgiSurface(surface.Get(), &properties,
                                                        &pageBitmap))) {
            return E_FAIL;
        }
        context->SetTarget(pageBitmap.Get());
        context->BeginDraw();
        drawPage(context.Get(), createImageBitmap(context.Get(), *image_).Get(), layout,
                 D2D1::Matrix3x2F::Scale(scale, scale));
        const HRESULT drawn = context->EndDraw();
        context->SetTarget(nullptr);  // 割り当てを外してから渡す(受け手が読めるように)
        if (FAILED(drawn)) return drawn;

        const UINT32 pageNumber =
            desiredJobPage == JOB_PAGE_APPLICATION_DEFINED ? 1 : desiredJobPage;
        // プレビュー枠は 96dpi 合成なので、サーフェスの dpi もそのまま 96 で渡す
        return target_->DrawPage(pageNumber, surface.Get(), 96.0f, 96.0f);
    }

    /// printTaskOptions (IInspectable) から用紙の情報を取り出す
    static HRESULT pageDescriptionOf(IInspectable* printTaskOptions,
                                     wgp::PrintPageDescription& out) {
        if (!printTaskOptions) return E_INVALIDARG;
        ComPtr<wgp::IPrintTaskOptionsCore> core;
        const HRESULT hr = printTaskOptions->QueryInterface(IID_PPV_ARGS(&core));
        if (FAILED(hr)) return hr;
        return core->GetPageDescription(1, &out);
    }

private:
    std::shared_ptr<const DecodedImage> image_;
    PrintOptions options_;
    std::shared_ptr<PrintDevice> device_;
    ComPtr<IPrintPreviewDxgiPackageTarget> target_;
    std::mutex mutex_;                            ///< description_ を守る
    wgp::PrintPageDescription description_{};     ///< Paginate で受け取った用紙の情報
    bool hasDescription_ = false;
};

/// OS へ渡す印刷ドキュメント。プレビューのページも本出力もここが入口になる
class PrintDocument final
    : public ComObject<wgp::IPrintDocumentSource, IPrintDocumentPageSource> {
public:
    PrintDocument(std::shared_ptr<const DecodedImage> image, const PrintOptions& options,
                  std::shared_ptr<PrintDevice> device, std::shared_ptr<PrintProgress> progress)
        : image_(std::move(image)),
          options_(options),
          device_(std::move(device)),
          progress_(std::move(progress)) {}

    HRESULT STDMETHODCALLTYPE GetPreviewPageCollection(
        IPrintDocumentPackageTarget* target,
        IPrintPreviewPageCollection** pages) noexcept override {
        if (!target || !pages) return E_POINTER;
        *pages = nullptr;
        ComPtr<IPrintPreviewDxgiPackageTarget> dxgiTarget;
        const HRESULT hr = target->GetPackageTarget(
            ID_PREVIEWPACKAGETARGET_DXGI, IID_PPV_ARGS(dxgiTarget.GetAddressOf()));
        if (FAILED(hr)) return hr;
        *pages = new (std::nothrow) PreviewPages(image_, options_, device_, dxgiTarget);
        return *pages ? S_OK : E_OUTOFMEMORY;
    }

    HRESULT STDMETHODCALLTYPE MakeDocument(IInspectable* printTaskOptions,
                                           IPrintDocumentPackageTarget* target) noexcept override {
        if (!target) return E_POINTER;
        // ここへ来たということは利用者が「印刷」を押した(取りやめとの区別に使う)
        progress_->documentRequested = true;

        wgp::PrintPageDescription description{};
        HRESULT hr = PreviewPages::pageDescriptionOf(printTaskOptions, description);
        if (FAILED(hr)) return hr;
        const PageLayout layout =
            computePageLayout(description, image_->width, image_->height, options_);
        if (!layout.valid) return E_FAIL;

        IWICImagingFactory* wic = wicFactoryForThisThread();
        if (!wic) return E_FAIL;
        ComPtr<ID2D1PrintControl> printControl;
        hr = device_->device->CreatePrintControl(wic, target, nullptr, &printControl);
        if (FAILED(hr)) return hr;

        ComPtr<ID2D1DeviceContext> context;
        hr = device_->device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &context);
        if (FAILED(hr)) return hr;
        ComPtr<ID2D1CommandList> commands;
        hr = context->CreateCommandList(&commands);
        if (FAILED(hr)) return hr;

        context->SetTarget(commands.Get());
        context->BeginDraw();
        drawPage(context.Get(), createImageBitmap(context.Get(), *image_).Get(), layout,
                 D2D1::Matrix3x2F::Identity());
        hr = context->EndDraw();
        if (FAILED(hr)) return hr;
        hr = commands->Close();
        if (FAILED(hr)) return hr;

        hr = printControl->AddPage(commands.Get(),
                                   D2D1::SizeF(layout.pageWidth, layout.pageHeight), nullptr);
        if (FAILED(hr)) {
            printControl->Close();
            return hr;
        }
        return printControl->Close();
    }

private:
    std::shared_ptr<const DecodedImage> image_;
    PrintOptions options_;
    std::shared_ptr<PrintDevice> device_;
    std::shared_ptr<PrintProgress> progress_;
};

/// 印刷タスクの「ソースをくれ」に応えるデリゲート
class SourceRequestedHandler final : public ComObject<wgp::IPrintTaskSourceRequestedHandler> {
public:
    explicit SourceRequestedHandler(ComPtr<wgp::IPrintDocumentSource> source)
        : source_(std::move(source)) {}

    HRESULT STDMETHODCALLTYPE Invoke(wgp::IPrintTaskSourceRequestedArgs* args) noexcept override {
        if (!args) return E_POINTER;
        return args->SetSource(source_.Get());
    }

private:
    ComPtr<wgp::IPrintDocumentSource> source_;
};

/// 印刷タスクの完了(送信・取りやめ・失敗)を受け取るデリゲート
class TaskCompletedHandler final : public ComObject<PrintTaskCompletedHandler> {
public:
    explicit TaskCompletedHandler(std::shared_ptr<PrintProgress> progress)
        : progress_(std::move(progress)) {}

    HRESULT STDMETHODCALLTYPE Invoke(wgp::IPrintTask*,
                                     wgp::IPrintTaskCompletedEventArgs* args) noexcept override {
        wgp::PrintTaskCompletion completion = wgp::PrintTaskCompletion_Failed;
        if (args && SUCCEEDED(args->get_Completion(&completion))) {
            progress_->completion = static_cast<int>(completion);
        }
        return S_OK;
    }

private:
    std::shared_ptr<PrintProgress> progress_;
};

/// 印刷 UI から「印刷対象をくれ」と言われたときに印刷タスクを作るデリゲート
class TaskRequestedHandler final : public ComObject<PrintTaskRequestedHandler> {
public:
    TaskRequestedHandler(ComPtr<wgp::IPrintDocumentSource> source, std::wstring title,
                         std::shared_ptr<PrintProgress> progress)
        : source_(std::move(source)), title_(std::move(title)), progress_(std::move(progress)) {}

    HRESULT STDMETHODCALLTYPE Invoke(wgp::IPrintManager*,
                                     wgp::IPrintTaskRequestedEventArgs* args) noexcept override {
        if (!args) return E_POINTER;
        ComPtr<wgp::IPrintTaskRequest> request;
        HRESULT hr = args->get_Request(&request);
        if (FAILED(hr)) return hr;

        const HString title(title_);
        const ComPtr<SourceRequestedHandler> sourceHandler = new (std::nothrow)
            SourceRequestedHandler(source_);
        if (!sourceHandler) return E_OUTOFMEMORY;
        ComPtr<wgp::IPrintTask> task;
        hr = request->CreatePrintTask(title.get(), sourceHandler.Get(), &task);
        if (FAILED(hr)) return hr;
        progress_->taskCreated = true;

        const ComPtr<TaskCompletedHandler> completed = new (std::nothrow)
            TaskCompletedHandler(progress_);
        if (!completed) return E_OUTOFMEMORY;
        EventRegistrationToken token{};
        return task->add_Completed(completed.Get(), &token);
    }

private:
    ComPtr<wgp::IPrintDocumentSource> source_;
    std::wstring title_;
    std::shared_ptr<PrintProgress> progress_;
};

/// ShowPrintUIForWindowAsync の完了を受け取るデリゲート。
/// **これは「ダイアログを出せた」の合図で、閉じた合図ではない**(閉じたことは
/// 印刷タスクの Completed で分かる)。ここでは失敗を検知して従来経路へ落とすのに使う
class ShowUiCompletedHandler final : public ComObject<ShowPrintUiHandler> {
public:
    explicit ShowUiCompletedHandler(std::shared_ptr<PrintProgress> progress)
        : progress_(std::move(progress)) {}

    HRESULT STDMETHODCALLTYPE Invoke(ShowPrintUiOperation* operation,
                                     wf::AsyncStatus status) noexcept override {
        boolean shown = false;
        if (status == wf::AsyncStatus::Completed && operation &&
            SUCCEEDED(operation->GetResults(&shown))) {
            progress_->uiSucceeded = shown != false;
        }
        progress_->uiDone = true;
        return S_OK;
    }

private:
    std::shared_ptr<PrintProgress> progress_;
};

/**
 * 条件が満たされるまでこのスレッドのメッセージを回す。
 *
 * 印刷 UI も、プレビューの描画要求も、このスレッドのメッセージループを通って来る。
 * ここで単純にブロックすると、ダイアログが出たまま何も進まなくなる。
 *
 * @return 条件が満たされたら true。時間切れ・WM_QUIT なら false。
 */
template <typename Predicate>
bool pumpUntil(Predicate done, const DWORD timeoutMs) {
    const ULONGLONG start = GetTickCount64();
    while (!done()) {
        if (GetTickCount64() - start >= timeoutMs) return false;
        // 何か届くまで待つ(空回しで CPU を焼かない)。50ms で目を覚ますのは、
        // 完了フラグが別スレッドで立つ場合にメッセージが来ないことがあるため
        MsgWaitForMultipleObjectsEx(0, nullptr, 50, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                // アプリ終了。本体のループにも伝える
                PostQuitMessage(static_cast<int>(msg.wParam));
                return false;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return true;
}

} // namespace

ModernPrintStatus printWithModernUi(HWND owner, const DecodedImage& image,
                                    const std::wstring& jobName, const PrintOptions& options) {
    // 印刷 UI はウィンドウに紐づく (IPrintManagerInterop)。親が無ければ従来経路へ
    if (!owner || image.width == 0 || image.height == 0) return ModernPrintStatus::Unavailable;
    if (!comBase().ok) return ModernPrintStatus::Unavailable;

    // UI スレッドは OleInitialize 済みの STA。WinRT もその上で初期化する
    // (印刷 UI はメッセージループを持つスレッドを要求する)
    const HRESULT initialized = comBase().roInitialize(RO_INIT_SINGLETHREADED);
    if (FAILED(initialized)) return ModernPrintStatus::Unavailable;
    struct RoScope {
        ~RoScope() { comBase().roUninitialize(); }
    } roScope;

    ComPtr<IPrintManagerInterop> interop;
    if (FAILED(activationFactory(RuntimeClass_Windows_Graphics_Printing_PrintManager, interop))) {
        return ModernPrintStatus::Unavailable;
    }
    ComPtr<wgp::IPrintManager> manager;
    if (FAILED(interop->GetForWindow(owner, IID_PPV_ARGS(manager.GetAddressOf())))) {
        return ModernPrintStatus::Unavailable;
    }
    const std::shared_ptr<PrintDevice> device = createPrintDevice();
    if (!device) return ModernPrintStatus::Unavailable;

    // 印刷は UI からの制御を離れて続くことがある(スプールは非同期)。画像は複製して
    // ドキュメント側へ渡し、呼び出し元の寿命に依存しないようにする
    auto copy = std::make_shared<DecodedImage>(image);
    const auto progress = std::make_shared<PrintProgress>();
    const ComPtr<PrintDocument> document =
        new (std::nothrow) PrintDocument(copy, options, device, progress);
    if (!document) return ModernPrintStatus::Unavailable;
    ComPtr<wgp::IPrintDocumentSource> source;
    if (FAILED(document.As(&source))) return ModernPrintStatus::Unavailable;

    const ComPtr<TaskRequestedHandler> requested =
        new (std::nothrow) TaskRequestedHandler(source, jobName, progress);
    if (!requested) return ModernPrintStatus::Unavailable;
    EventRegistrationToken token{};
    if (FAILED(manager->add_PrintTaskRequested(requested.Get(), &token))) {
        return ModernPrintStatus::Unavailable;
    }

    ComPtr<ShowPrintUiOperation> operation;
    HRESULT hr = interop->ShowPrintUIForWindowAsync(owner, IID_PPV_ARGS(operation.GetAddressOf()));
    if (SUCCEEDED(hr)) {
        const ComPtr<ShowUiCompletedHandler> completed =
            new (std::nothrow) ShowUiCompletedHandler(progress);
        hr = completed ? operation->put_Completed(completed.Get()) : E_OUTOFMEMORY;
    }
    if (FAILED(hr)) {
        manager->remove_PrintTaskRequested(token);
        return ModernPrintStatus::Unavailable;
    }

    // ダイアログが出ている間は本体のウィンドウを無効にする(PrintDlg と同じ扱い)。
    // 無効にしないと、印刷の裏で画像を切り替えるなどの操作ができてしまう
    EnableWindow(owner, FALSE);
    // ShowPrintUIForWindowAsync は「ダイアログを出せた」時点で完了する(閉じるのを
    // 待つものではない)。終わりの合図は印刷タスクの Completed のほうで、
    // 送信・取りやめ・失敗のいずれでも来る
    pumpUntil(
        [&] {
            if (progress->completion.load() >= 0) return true;
            // ダイアログを出せなかった場合はここで諦める(従来経路へ落とす)
            return progress->uiDone.load() && !progress->uiSucceeded.load() &&
                   !progress->taskCreated.load();
        },
        kUiTimeoutMs);
    EnableWindow(owner, TRUE);
    SetActiveWindow(owner);
    manager->remove_PrintTaskRequested(token);

    if (const int completion = progress->completion.load(); completion >= 0) {
        switch (static_cast<wgp::PrintTaskCompletion>(completion)) {
        case wgp::PrintTaskCompletion_Submitted:
            return ModernPrintStatus::Printed;
        case wgp::PrintTaskCompletion_Canceled:
            return ModernPrintStatus::Canceled;
        default:
            return ModernPrintStatus::Failed;  // Failed / Abandoned
        }
    }
    // 完了通知はスプールの後なので、間に合わないことがある。MakeDocument が来ていれば
    // 送ったとみなす(ここまで来たら印刷は OS 側で続く)
    if (progress->documentRequested.load()) return ModernPrintStatus::Printed;
    // ダイアログ自体が出なかった場合だけ、従来の印刷ダイアログへ落とす
    if (!progress->taskCreated.load()) return ModernPrintStatus::Unavailable;
    return ModernPrintStatus::Canceled;
}

} // namespace blinker
