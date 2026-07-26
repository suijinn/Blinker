#include "win/ocr_winrt.h"

#include <windows.h>

#include <wincodec.h>
#include <wrl/client.h>

// WinRT を C++/WinRT ではなく ABI (MIDL 生成ヘッダ) で直接使う。windowsapp.lib への
// 静的リンクを避け、exe のインポートを増やさないため
#include <roapi.h>
#include <winstring.h>

#include <windows.foundation.h>
#include <windows.globalization.h>
#include <windows.graphics.imaging.h>
#include <windows.media.ocr.h>
#include <windows.security.cryptography.h>

#include <algorithm>
#include <atomic>
#include <format>
#include <string_view>
#include <vector>

#include "core/ocr_text.h"
#include "core/unicode.h"
#include "win/wic_factory.h"

namespace blinker {
namespace {

using Microsoft::WRL::ComPtr;

namespace wf = ABI::Windows::Foundation;
namespace wg = ABI::Windows::Globalization;
namespace wgi = ABI::Windows::Graphics::Imaging;
namespace wmo = ABI::Windows::Media::Ocr;
namespace wsc = ABI::Windows::Security::Cryptography;
namespace wss = ABI::Windows::Storage::Streams;

using OcrOperation = wf::IAsyncOperation<wmo::OcrResult*>;
using OcrHandler = wf::IAsyncOperationCompletedHandler<wmo::OcrResult*>;
using LanguageVectorView = wf::Collections::IVectorView<wg::Language*>;

// 認識が終わらない場合でもワーカースレッドを永久に止めない上限。
// 実測では 4K のスクリーンショットでも 1 秒程度で返る
constexpr DWORD kRecognizeTimeoutMs = 60'000;

// 拡大して読み直すかの判断は core の ocrRetryUpscale が持つ(単体テスト対象)。
// 認識器がサイズ上限を報告しなかった場合に渡す「上限なし」の値
constexpr double kUnlimitedUpscale = 1e9;

/**
 * combase.dll の遅延解決。
 *
 * 静的リンク (windowsapp.lib) を避けることで、exe のインポートテーブルが増えず、
 * OCR を一度も使わなければ combase.dll が読み込まれることもない(起動時間を守る)。
 */
struct ComBaseApi {
    HRESULT(WINAPI* roInitialize)(RO_INIT_TYPE) = nullptr;
    void(WINAPI* roUninitialize)() = nullptr;
    HRESULT(WINAPI* roGetActivationFactory)(HSTRING, REFIID, void**) = nullptr;
    HRESULT(WINAPI* windowsCreateString)(PCNZWCH, UINT32, HSTRING*) = nullptr;
    HRESULT(WINAPI* windowsDeleteString)(HSTRING) = nullptr;
    PCWSTR(WINAPI* windowsGetStringRawBuffer)(HSTRING, UINT32*) = nullptr;
    bool ok = false;
};

const ComBaseApi& comBase() {
    static const ComBaseApi api = [] {
        ComBaseApi a;
        // combase.dll は Windows 8 以降にのみ存在する。無ければ ok = false のまま
        HMODULE module = LoadLibraryExW(L"combase.dll", nullptr,
                                        LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) return a;
        const auto load = [module](const char* name) {
            return reinterpret_cast<void*>(GetProcAddress(module, name));
        };
        a.roInitialize = reinterpret_cast<decltype(a.roInitialize)>(load("RoInitialize"));
        a.roUninitialize = reinterpret_cast<decltype(a.roUninitialize)>(load("RoUninitialize"));
        a.roGetActivationFactory =
            reinterpret_cast<decltype(a.roGetActivationFactory)>(load("RoGetActivationFactory"));
        a.windowsCreateString =
            reinterpret_cast<decltype(a.windowsCreateString)>(load("WindowsCreateString"));
        a.windowsDeleteString =
            reinterpret_cast<decltype(a.windowsDeleteString)>(load("WindowsDeleteString"));
        a.windowsGetStringRawBuffer = reinterpret_cast<decltype(a.windowsGetStringRawBuffer)>(
            load("WindowsGetStringRawBuffer"));
        a.ok = a.roInitialize && a.roUninitialize && a.roGetActivationFactory &&
               a.windowsCreateString && a.windowsDeleteString && a.windowsGetStringRawBuffer;
        return a;
    }();
    return api;
}

/// HSTRING の所有権を持つ薄いラッパ(WindowsCreateString / WindowsDeleteString の対)。
class HString {
public:
    HString() = default;

    explicit HString(std::wstring_view s) {
        if (!comBase().ok) return;
        comBase().windowsCreateString(s.data(), static_cast<UINT32>(s.size()), &value_);
    }

    ~HString() {
        if (value_ && comBase().ok) comBase().windowsDeleteString(value_);
    }

    HString(const HString&) = delete;
    HString& operator=(const HString&) = delete;

    HSTRING get() const { return value_; }
    HSTRING* put() { return &value_; }

    /// 保持している文字列を UTF-8 で返す(空・未設定なら空文字列)。
    std::string toUtf8() const {
        if (!value_ || !comBase().ok) return {};
        UINT32 length = 0;
        const wchar_t* buffer = comBase().windowsGetStringRawBuffer(value_, &length);
        if (!buffer) return {};
        return wideToUtf8(std::wstring_view(buffer, length));
    }

private:
    HSTRING value_ = nullptr;
};

/// スレッドごとの WinRT (MTA) 初期化。ワーカースレッドから最初に呼ばれた時点で走る。
bool winRtReadyForThisThread() {
    if (!comBase().ok) return false;
    thread_local struct ThreadState {
        bool initialized = false;
        bool usable = false;

        ThreadState() {
            const HRESULT hr = comBase().roInitialize(RO_INIT_MULTITHREADED);
            // RPC_E_CHANGED_MODE は「このスレッドは既に STA」。STA で非同期完了を
            // ブロック待ちするとデッドロックするため、使えないものとして扱う
            initialized = SUCCEEDED(hr);
            usable = initialized || hr == S_FALSE;
        }
        ~ThreadState() {
            if (initialized) comBase().roUninitialize();
        }
    } state;
    return state.usable;
}

/**
 * アクティベーションファクトリを取得する。
 * @param classId ランタイムクラス名。
 * @param out     受け取り先。
 */
template <typename T>
HRESULT activationFactory(const wchar_t* classId, ComPtr<T>& out) {
    if (!comBase().ok) return E_NOINTERFACE;
    const HString name(classId);
    if (!name.get()) return E_OUTOFMEMORY;
    return comBase().roGetActivationFactory(name.get(), __uuidof(T),
                                            reinterpret_cast<void**>(out.GetAddressOf()));
}

/**
 * RecognizeAsync の完了を待つためのデリゲート。
 *
 * ヒープに置き、非同期操作と待ち側の両方が参照を持つ(待ちがタイムアウトしても
 * 遅れて届く Invoke が解放済みのイベントに触らないようにするため)。
 */
class CompletedHandler final : public OcrHandler {
public:
    CompletedHandler() : event_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(OcrHandler)) {
            *ppv = static_cast<OcrHandler*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++refCount_; }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --refCount_;
        if (remaining == 0) delete this;
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE Invoke(OcrOperation*, wf::AsyncStatus status) override {
        status_ = status;
        if (event_) SetEvent(event_);
        return S_OK;
    }

    /// 完了を待つ。@return 完了したら true、タイムアウト・イベント生成失敗なら false。
    bool wait(DWORD timeoutMs) const {
        if (!event_) return false;
        return WaitForSingleObject(event_, timeoutMs) == WAIT_OBJECT_0;
    }

    wf::AsyncStatus status() const { return status_; }

    // Release で delete this するため、外から delete しないこと
    // (このファイル内でしか作らないので public のままにしてある)。
    // IUnknown は仮想デストラクタを持たないため override は付かない
    ~CompletedHandler() {
        if (event_) CloseHandle(event_);
    }

private:
    std::atomic<ULONG> refCount_{1};
    HANDLE event_ = nullptr;
    // Invoke はスレッドプールから呼ばれ、wait 側とは SetEvent/WaitForSingleObject で
    // 順序付けされるが、読み書きの競合自体を避けるため atomic にしておく
    std::atomic<wf::AsyncStatus> status_{wf::AsyncStatus::Started};
};

void setError(std::string* error, std::string_view stage, HRESULT hr) {
    if (error) *error = std::format("{} (0x{:08X})", stage, static_cast<uint32_t>(hr));
}

void setError(std::string* error, std::string reason) {
    if (error) *error = std::move(reason);
}

/// WIC で拡大・縮小する(Fant = 面積平均。拡大では双三次相当の滑らかさになる)。
std::shared_ptr<DecodedImage> resampleImage(const DecodedImage& src, uint32_t newWidth,
                                            uint32_t newHeight, std::string* error) {
    IWICImagingFactory* factory = wicFactoryForThisThread();
    if (!factory) {
        setError(error, "WICファクトリ生成");
        return nullptr;
    }

    ComPtr<IWICBitmap> source;
    HRESULT hr = factory->CreateBitmapFromMemory(
        src.width, src.height, GUID_WICPixelFormat32bppPBGRA, src.width * 4,
        static_cast<UINT>(src.pixels.size()), const_cast<BYTE*>(src.pixels.data()), &source);
    if (FAILED(hr)) {
        setError(error, "拡縮用ビットマップ生成", hr);
        return nullptr;
    }

    ComPtr<IWICBitmapScaler> scaler;
    hr = factory->CreateBitmapScaler(&scaler);
    if (SUCCEEDED(hr)) {
        hr = scaler->Initialize(source.Get(), newWidth, newHeight,
                                WICBitmapInterpolationModeFant);
    }
    if (FAILED(hr)) {
        setError(error, std::format("拡縮 {} x {} → {} x {}", src.width, src.height, newWidth,
                                    newHeight),
                 hr);
        return nullptr;
    }

    auto result = std::make_shared<DecodedImage>();
    result->width = newWidth;
    result->height = newHeight;
    result->pixels.resize(static_cast<size_t>(newWidth) * newHeight * 4);
    hr = scaler->CopyPixels(nullptr, newWidth * 4, static_cast<UINT>(result->pixels.size()),
                            result->pixels.data());
    if (FAILED(hr)) {
        setError(error, "拡縮結果の取得", hr);
        return nullptr;
    }
    return result;
}

/// 認識器が受け付ける大きさへ縮小する。縮小不要なら nullptr を返し scale は 1 のまま。
std::shared_ptr<DecodedImage> downscaleForOcr(const DecodedImage& src, uint32_t maxDimension,
                                              double* scale, std::string* error) {
    *scale = 1.0;
    if (maxDimension == 0) return nullptr;
    if (src.width <= maxDimension && src.height <= maxDimension) return nullptr;

    const double factor = std::min(static_cast<double>(maxDimension) / src.width,
                                   static_cast<double>(maxDimension) / src.height);
    const uint32_t newWidth = std::max(1u, static_cast<uint32_t>(src.width * factor));
    const uint32_t newHeight = std::max(1u, static_cast<uint32_t>(src.height * factor));
    auto result = resampleImage(src, newWidth, newHeight, error);
    if (!result) return nullptr;
    *scale = 1.0 / factor;  // 認識座標を元の画像スケールへ戻す係数
    return result;
}

/// 認識された行の高さの中央値(認識に使ったビットマップのピクセル)。行が無ければ 0。
int medianLineHeight(const OcrResult& result, double coordScale) {
    std::vector<int> heights;
    heights.reserve(result.lines.size());
    for (const OcrLine& line : result.lines) {
        if (line.bounds.h > 0) heights.push_back(line.bounds.h);
    }
    if (heights.empty()) return 0;
    std::nth_element(heights.begin(), heights.begin() + heights.size() / 2, heights.end());
    const int medianInOriginal = heights[heights.size() / 2];
    // bounds は元画像スケールへ直してあるので、ビットマップ上の高さへ戻す
    return static_cast<int>(medianInOriginal / coordScale);
}

/// DecodedImage から SoftwareBitmap を作る。
ComPtr<wgi::ISoftwareBitmap> makeSoftwareBitmap(const DecodedImage& image, std::string* error) {
    ComPtr<wsc::ICryptographicBufferStatics> bufferStatics;
    HRESULT hr = activationFactory(RuntimeClass_Windows_Security_Cryptography_CryptographicBuffer,
                                   bufferStatics);
    if (FAILED(hr)) {
        setError(error, "CryptographicBuffer の取得", hr);
        return nullptr;
    }

    ComPtr<wss::IBuffer> buffer;
    hr = bufferStatics->CreateFromByteArray(static_cast<UINT32>(image.pixels.size()),
                                            const_cast<BYTE*>(image.pixels.data()), &buffer);
    if (FAILED(hr)) {
        setError(error, "画素バッファ生成", hr);
        return nullptr;
    }

    ComPtr<wgi::ISoftwareBitmapStatics> bitmapStatics;
    hr = activationFactory(RuntimeClass_Windows_Graphics_Imaging_SoftwareBitmap, bitmapStatics);
    if (FAILED(hr)) {
        setError(error, "SoftwareBitmap の取得", hr);
        return nullptr;
    }

    // 呼び出し側で背景へ焼き込み済みなので、アルファは無視してよい
    ComPtr<wgi::ISoftwareBitmap> bitmap;
    hr = bitmapStatics->CreateCopyWithAlphaFromBuffer(
        buffer.Get(), wgi::BitmapPixelFormat_Bgra8, static_cast<INT32>(image.width),
        static_cast<INT32>(image.height), wgi::BitmapAlphaMode_Ignore, &bitmap);
    if (FAILED(hr)) {
        setError(error, std::format("SoftwareBitmap 生成 ({} x {})", image.width, image.height),
                 hr);
        return nullptr;
    }
    return bitmap;
}

/// 認識結果を core 側の型へ移す。座標は scale 倍して元の画像スケールへ戻す。
bool convertResult(wmo::IOcrResult* raw, double scale, OcrResult* out, std::string* error) {
    ComPtr<wf::Collections::IVectorView<wmo::OcrLine*>> lines;
    HRESULT hr = raw->get_Lines(&lines);
    if (FAILED(hr)) {
        setError(error, "認識行の取得", hr);
        return false;
    }
    unsigned lineCount = 0;
    hr = lines->get_Size(&lineCount);
    if (FAILED(hr)) {
        setError(error, "認識行数の取得", hr);
        return false;
    }

    out->lines.clear();
    out->lines.reserve(lineCount);
    for (unsigned i = 0; i < lineCount; ++i) {
        ComPtr<wmo::IOcrLine> line;
        if (FAILED(lines->GetAt(i, &line)) || !line) continue;

        OcrLine converted;
        HString text;
        if (SUCCEEDED(line->get_Text(text.put()))) converted.text = text.toUtf8();

        // 行の矩形はエンジンが返さないため、単語の矩形の和として組み立てる。
        // なお行のテキストを OcrLine::Text ではなく OcrWord の空白連結で作っても
        // 結果は 1 文字も変わらない(実測)。"OCR World" が "OCRWorld" になるのは
        // 認識器が 2 語を 1 単語として返すためで、連結側では直せない
        ComPtr<wf::Collections::IVectorView<wmo::OcrWord*>> words;
        unsigned wordCount = 0;
        if (SUCCEEDED(line->get_Words(&words)) && words &&
            SUCCEEDED(words->get_Size(&wordCount))) {
            bool first = true;
            double left = 0, top = 0, right = 0, bottom = 0;
            for (unsigned w = 0; w < wordCount; ++w) {
                ComPtr<wmo::IOcrWord> word;
                if (FAILED(words->GetAt(w, &word)) || !word) continue;
                wf::Rect rect{};
                if (FAILED(word->get_BoundingRect(&rect))) continue;
                const double x0 = rect.X * scale;
                const double y0 = rect.Y * scale;
                const double x1 = (rect.X + rect.Width) * scale;
                const double y1 = (rect.Y + rect.Height) * scale;
                if (first) {
                    left = x0;
                    top = y0;
                    right = x1;
                    bottom = y1;
                    first = false;
                } else {
                    left = std::min(left, x0);
                    top = std::min(top, y0);
                    right = std::max(right, x1);
                    bottom = std::max(bottom, y1);
                }
            }
            if (!first) {
                converted.bounds.x = static_cast<int>(left);
                converted.bounds.y = static_cast<int>(top);
                converted.bounds.w = static_cast<int>(right - left);
                converted.bounds.h = static_cast<int>(bottom - top);
            }
        }
        out->lines.push_back(std::move(converted));
    }
    return true;
}

/// 認識器が使える言語を "ja, en-US" の形で並べる(設定を書く助けとして表示する)。
std::string availableLanguages(wmo::IOcrEngineStatics* statics) {
    ComPtr<LanguageVectorView> languages;
    if (FAILED(statics->get_AvailableRecognizerLanguages(&languages)) || !languages) return {};
    unsigned count = 0;
    if (FAILED(languages->get_Size(&count))) return {};

    std::string result;
    for (unsigned i = 0; i < count; ++i) {
        ComPtr<wg::ILanguage> language;
        if (FAILED(languages->GetAt(i, &language)) || !language) continue;
        HString tag;
        if (FAILED(language->get_LanguageTag(tag.put()))) continue;
        if (!result.empty()) result += ", ";
        result += tag.toUtf8();
    }
    return result;
}

/// 先頭の認識器言語でエンジンを作る(利用者の表示言語に認識器が無いときの受け皿)。
ComPtr<wmo::IOcrEngine> createFromFirstAvailable(wmo::IOcrEngineStatics* statics) {
    ComPtr<LanguageVectorView> languages;
    if (FAILED(statics->get_AvailableRecognizerLanguages(&languages)) || !languages) return nullptr;
    unsigned count = 0;
    if (FAILED(languages->get_Size(&count))) return nullptr;
    for (unsigned i = 0; i < count; ++i) {
        ComPtr<wg::ILanguage> language;
        if (FAILED(languages->GetAt(i, &language)) || !language) continue;
        ComPtr<wmo::IOcrEngine> engine;
        if (SUCCEEDED(statics->TryCreateFromLanguage(language.Get(), &engine)) && engine) {
            return engine;
        }
    }
    return nullptr;
}

} // namespace

OcrEngineWinrt::OcrEngineWinrt(std::string languageTag) : languageTag_(std::move(languageTag)) {}

OcrEngineWinrt::~OcrEngineWinrt() {
    // WinRT の in-proc オブジェクトはフリースレッドなので、生成したスレッド
    // (OcrService のワーカー)と別のスレッドから解放してよい
    if (engine_) static_cast<wmo::IOcrEngine*>(engine_)->Release();
}

bool OcrEngineWinrt::ensureEngineLocked(std::string* error) {
    if (initialized_) {
        if (!engine_) setError(error, initError_);
        return engine_ != nullptr;
    }
    initialized_ = true;  // 失敗しても再試行しない(毎回同じ理由で数百 ms を捨てないため)

    if (!comBase().ok) {
        initError_ = "この環境では文字認識を利用できません (WinRT 非対応)";
        setError(error, initError_);
        return false;
    }
    if (!winRtReadyForThisThread()) {
        initError_ = "文字認識を初期化できません (スレッドの初期化に失敗)";
        setError(error, initError_);
        return false;
    }

    ComPtr<wmo::IOcrEngineStatics> statics;
    HRESULT hr = activationFactory(RuntimeClass_Windows_Media_Ocr_OcrEngine, statics);
    if (FAILED(hr) || !statics) {
        initError_ = std::format("文字認識を利用できません (0x{:08X})", static_cast<uint32_t>(hr));
        setError(error, initError_);
        return false;
    }

    ComPtr<wmo::IOcrEngine> engine;
    if (!languageTag_.empty()) {
        // blinker.ini の [ocr] language で指定された言語を使う
        ComPtr<wg::ILanguageFactory> languageFactory;
        if (SUCCEEDED(activationFactory(RuntimeClass_Windows_Globalization_Language,
                                        languageFactory))) {
            const HString tag(utf8ToWide(languageTag_));
            ComPtr<wg::ILanguage> language;
            if (SUCCEEDED(languageFactory->CreateLanguage(tag.get(), &language)) && language) {
                statics->TryCreateFromLanguage(language.Get(), &engine);
            }
        }
        if (!engine) {
            initError_ = std::format("言語 \"{}\" の認識器がありません (利用できる言語: {})",
                                     languageTag_, availableLanguages(statics.Get()));
            setError(error, initError_);
            return false;
        }
    } else {
        // 表示言語から選び、認識器が無ければ入っている言語のどれかで動かす
        statics->TryCreateFromUserProfileLanguages(&engine);
        if (!engine) engine = createFromFirstAvailable(statics.Get());
        if (!engine) {
            initError_ =
                "文字認識の言語パックが入っていません "
                "(設定 → 言語と地域 で言語のオプションから追加できます)";
            setError(error, initError_);
            return false;
        }
    }

    // 認識器が受け付ける大きさの上限。超える画像は縮小してから渡す
    statics->get_MaxImageDimension(&maxDimension_);

    ComPtr<wg::ILanguage> recognizerLanguage;
    if (SUCCEEDED(engine->get_RecognizerLanguage(&recognizerLanguage)) && recognizerLanguage) {
        HString tag;
        if (SUCCEEDED(recognizerLanguage->get_LanguageTag(tag.put()))) {
            engineLanguage_ = tag.toUtf8();
        }
    }

    engine_ = engine.Detach();
    return true;
}

bool OcrEngineWinrt::recognizeOnceLocked(const DecodedImage& bitmapSource, double coordScale,
                                         OcrResult* result, std::string* error) {
    const ComPtr<wgi::ISoftwareBitmap> bitmap = makeSoftwareBitmap(bitmapSource, error);
    if (!bitmap) return false;

    ComPtr<OcrOperation> operation;
    HRESULT hr = static_cast<wmo::IOcrEngine*>(engine_)->RecognizeAsync(bitmap.Get(), &operation);
    if (FAILED(hr) || !operation) {
        setError(error, "認識の開始", hr);
        return false;
    }

    // 完了はスレッドプールのスレッドから通知される。ここは MTA なので待ってよい。
    // 生成時の参照カウント 1 をそのまま受け取るため Attach を使う
    ComPtr<CompletedHandler> handler;
    handler.Attach(new (std::nothrow) CompletedHandler());
    if (!handler) {
        setError(error, "完了ハンドラの生成に失敗");
        return false;
    }

    hr = operation->put_Completed(handler.Get());
    if (FAILED(hr)) {
        setError(error, "完了ハンドラの登録", hr);
        return false;
    }
    if (!handler->wait(kRecognizeTimeoutMs)) {
        setError(error, "認識が時間内に終わりませんでした");
        return false;
    }
    if (handler->status() != wf::AsyncStatus::Completed) {
        setError(error, "認識に失敗しました");
        return false;
    }

    ComPtr<wmo::IOcrResult> raw;
    hr = operation->GetResults(&raw);
    if (FAILED(hr) || !raw) {
        setError(error, "認識結果の取得", hr);
        return false;
    }
    return convertResult(raw.Get(), coordScale, result, error);
}

bool OcrEngineWinrt::recognize(const DecodedImage& image, OcrResult* result, std::string* error) {
    if (!result) return false;
    if (image.width == 0 || image.height == 0 || image.pixels.empty()) {
        setError(error, "認識する画像がありません");
        return false;
    }

    std::lock_guard lock(mutex_);
    if (!winRtReadyForThisThread()) {
        setError(error, "文字認識を初期化できません (スレッドの初期化に失敗)");
        return false;
    }
    if (!ensureEngineLocked(error)) return false;

    // 上限を超える画像は認識器が受け付けないので先に縮小する
    double coordScale = 1.0;
    std::string scaleError;
    const std::shared_ptr<DecodedImage> shrunk =
        downscaleForOcr(image, maxDimension_, &coordScale, &scaleError);
    if (!shrunk && !scaleError.empty()) {
        setError(error, std::move(scaleError));
        return false;
    }
    const DecodedImage& source = shrunk ? *shrunk : image;

    if (!recognizeOnceLocked(source, coordScale, result, error)) return false;
    result->language = engineLanguage_;

    // 小さい文字は拡大すると精度が大きく上がる。どれだけ拡大すべきかは認識してみないと
    // 分からないので、1 回目の行高から決めて読み直す(判断は core の純関数が持つ)
    const double maxFactor =
        maxDimension_ == 0 ? kUnlimitedUpscale
                           : std::min(static_cast<double>(maxDimension_) / source.width,
                                      static_cast<double>(maxDimension_) / source.height);
    const double factor = ocrRetryUpscale(medianLineHeight(*result, coordScale), maxFactor);
    if (factor <= 1.0) return true;

    const auto enlarged = resampleImage(source, static_cast<uint32_t>(source.width * factor),
                                        static_cast<uint32_t>(source.height * factor), nullptr);
    if (!enlarged) return true;  // 拡大できなければ 1 回目の結果で十分

    OcrResult retry;
    if (!recognizeOnceLocked(*enlarged, coordScale / factor, &retry, nullptr)) return true;
    if (retry.lines.empty()) return true;  // 拡大して何も読めなくなったら 1 回目を採る

    retry.language = engineLanguage_;
    *result = std::move(retry);
    return true;
}

} // namespace blinker
