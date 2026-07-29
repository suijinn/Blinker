#include "win/decoder_wic.h"

#include <windows.h>

#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <format>
#include <new>
#include <numeric>
#include <string_view>
#include <vector>

#include "core/animation.h"
#include "core/exif.h"
#include "win/wic_factory.h"

namespace blinker {
namespace {

using Microsoft::WRL::ComPtr;

// 取り込む大きさの上限。これはメモリのための制限で、16384 x 16384 でも PBGRA で 1GB になる。
// 描画側 (GPU) の上限は機種によって更に低いことがあり、そちらは RendererD2D が
// GetMaximumBitmapSize を見て描画時に縮小する(元のピクセルは保存・コピー用に残す)
constexpr UINT kMaxDimension = 16384;

UINT16 readOrientation(IWICBitmapFrameDecode* frame) {
    ComPtr<IWICMetadataQueryReader> reader;
    if (FAILED(frame->GetMetadataQueryReader(&reader))) return 1;
    UINT16 orientation = 1;
    PROPVARIANT value;
    PropVariantInit(&value);
    // JPEG は /app1/ifd、TIFF 系は /ifd に格納される
    if (SUCCEEDED(reader->GetMetadataByName(L"/app1/ifd/{ushort=274}", &value)) &&
        value.vt == VT_UI2) {
        orientation = value.uiVal;
    } else {
        PropVariantClear(&value);
        if (SUCCEEDED(reader->GetMetadataByName(L"/ifd/{ushort=274}", &value)) &&
            value.vt == VT_UI2) {
            orientation = value.uiVal;
        }
    }
    PropVariantClear(&value);
    return orientation;
}

// 失敗した段階とコードを「段階 (0x........)」の形で記録する。
// 現物が手元にない不具合を切り分けられるよう、必ずどの段階で落ちたかを残す
void setError(std::string* error, std::string_view stage, HRESULT hr) {
    if (error) *error = std::format("{} (0x{:08X})", stage, static_cast<uint32_t>(hr));
}

/// コードを伴わない失敗(前提条件・上限超過など)を記録する。
void setError(std::string* error, const std::string& reason) {
    if (error) *error = reason;
}

// フレームの入力カラープロファイルを取り出す。変換が不要・取れない場合は nullptr。
//
// 諦める条件は「プロファイルが無い」「Exif の色空間が既に sRGB」。どちらも異常では
// ないので error は立てない(変換しないだけで、画像は問題なく表示される)
ComPtr<IWICColorContext> inputColorContext(IWICImagingFactory* factory,
                                           IWICBitmapFrameDecode* frame) {
    UINT count = 0;
    if (FAILED(frame->GetColorContexts(0, nullptr, &count)) || count == 0) return nullptr;

    std::vector<ComPtr<IWICColorContext>> contexts(count);
    std::vector<IWICColorContext*> raw(count);
    for (UINT i = 0; i < count; ++i) {
        if (FAILED(factory->CreateColorContext(&contexts[i]))) return nullptr;
        raw[i] = contexts[i].Get();
    }
    if (FAILED(frame->GetColorContexts(count, raw.data(), &count)) || count == 0) return nullptr;

    // 先頭を入力プロファイルとして使う(2 つ目以降が付くのは CMYK など特殊な場合)
    ComPtr<IWICColorContext> input = contexts[0];
    WICColorContextType type = WICColorContextUninitialized;
    if (SUCCEEDED(input->GetType(&type)) && type == WICColorContextExifColorSpace) {
        UINT space = 0;
        // Exif の色空間指定だけがある画像。1 = sRGB なら変換しても何も変わらない
        if (SUCCEEDED(input->GetExifColorSpace(&space)) && space == 1) return nullptr;
    }
    return input;
}

// 埋め込みプロファイルから sRGB へ変換する変換器を返す。変換が不要・できない場合は
// nullptr(呼び出し側は source をそのまま使う)。
// WIC がこの画素形式の変換に対応していない場合もここへ来るが、異常ではない
ComPtr<IWICBitmapSource> colorTransformToSrgb(IWICImagingFactory* factory,
                                              IWICBitmapFrameDecode* frame,
                                              IWICBitmapSource* source) {
    ComPtr<IWICColorContext> input = inputColorContext(factory, frame);
    if (!input) return nullptr;

    ComPtr<IWICColorContext> srgb;
    if (FAILED(factory->CreateColorContext(&srgb))) return nullptr;
    if (FAILED(srgb->InitializeFromExifColorSpace(1))) return nullptr;  // 1 = sRGB

    ComPtr<IWICColorTransform> transform;
    if (FAILED(factory->CreateColorTransformer(&transform))) return nullptr;
    // 出力は事前乗算でない BGRA。この後の IWICFormatConverter が PBGRA へ直す
    if (FAILED(transform->Initialize(source, input.Get(), srgb.Get(),
                                     GUID_WICPixelFormat32bppBGRA))) {
        return nullptr;
    }
    return transform;
}

// メタデータから符号なし整数を 1 つ読む。型は形式によって UI1 / UI2 / UI4 と揺れる
bool readMetadataUint(IWICMetadataQueryReader* reader, const wchar_t* name, unsigned& out) {
    if (!reader) return false;
    PROPVARIANT value;
    PropVariantInit(&value);
    bool ok = false;
    if (SUCCEEDED(reader->GetMetadataByName(name, &value))) {
        switch (value.vt) {
        case VT_UI1: out = value.bVal; ok = true; break;
        case VT_UI2: out = value.uiVal; ok = true; break;
        case VT_UI4: out = value.ulVal; ok = true; break;
        default: break;
        }
    }
    PropVariantClear(&value);
    return ok;
}

// GIF の繰り返し回数(NETSCAPE2.0 拡張)。0 = 無限。拡張が無ければ 1 回だけ再生
int readGifLoopCount(IWICMetadataQueryReader* reader) {
    if (!reader) return 1;
    PROPVARIANT value;
    PropVariantInit(&value);
    int loops = 1;
    if (SUCCEEDED(reader->GetMetadataByName(L"/appext/Data", &value)) &&
        value.vt == (VT_UI1 | VT_VECTOR) && value.caub.cElems >= 4 && value.caub.pElems[0] == 3) {
        // [ブロックサイズ=3][サブブロック ID=1][繰り返し回数 (リトルエンディアン 16bit)]
        loops = value.caub.pElems[2] | (value.caub.pElems[3] << 8);
    }
    PropVariantClear(&value);
    return loops;
}

// ICO の表示順(大きい順)。ファイル内の先頭は 16x16 のことが多く、そのまま出すと
// 「アイコンを開いたのに小さすぎる」ことになるため、既定で最大サイズを見せる
std::vector<UINT> icoFrameOrder(IWICBitmapDecoder* decoder, UINT frameCount) {
    std::vector<UINT> order(frameCount);
    std::iota(order.begin(), order.end(), 0u);
    std::vector<uint64_t> area(frameCount, 0);
    for (UINT i = 0; i < frameCount; ++i) {
        ComPtr<IWICBitmapFrameDecode> frame;
        UINT w = 0, h = 0;
        if (SUCCEEDED(decoder->GetFrame(i, &frame)) && SUCCEEDED(frame->GetSize(&w, &h))) {
            area[i] = static_cast<uint64_t>(w) * h;
        }
    }
    // 同じ大きさ(色数違い)はファイル内の順序を保つため stable_sort
    std::stable_sort(order.begin(), order.end(),
                     [&area](UINT a, UINT b) { return area[a] > area[b]; });
    return order;
}

// 表示順の index を WIC のフレーム番号へ直す。ICO 以外はそのまま
UINT resolveFrameIndex(IWICBitmapDecoder* decoder, UINT frameCount, UINT index) {
    GUID container{};
    if (FAILED(decoder->GetContainerFormat(&container)) ||
        container != GUID_ContainerFormatIco) {
        return index;
    }
    const std::vector<UINT> order = icoFrameOrder(decoder, frameCount);
    return index < order.size() ? order[index] : index;
}

// フレームを 32bpp PBGRA へ変換して取り出す(縮小・色変換・EXIF 回転は行わない)。
// アニメーションの各コマ用。GIF のコマは小さいので上限の心配がない
std::shared_ptr<DecodedImage> framePixels(IWICImagingFactory* factory,
                                          IWICBitmapFrameDecode* frame) {
    UINT width = 0;
    UINT height = 0;
    if (FAILED(frame->GetSize(&width, &height)) || width == 0 || height == 0) return nullptr;

    ComPtr<IWICFormatConverter> converter;
    HRESULT hr = factory->CreateFormatConverter(&converter);
    if (SUCCEEDED(hr)) {
        hr = converter->Initialize(frame, GUID_WICPixelFormat32bppPBGRA,
                                   WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeCustom);
    }
    if (FAILED(hr)) return nullptr;

    try {
        auto image = std::make_shared<DecodedImage>();
        image->width = width;
        image->height = height;
        image->pixels.resize(static_cast<size_t>(width) * height * 4);
        if (FAILED(converter->CopyPixels(nullptr, width * 4,
                                         static_cast<UINT>(image->pixels.size()),
                                         image->pixels.data()))) {
            return nullptr;
        }
        return image;
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
}

} // namespace

std::shared_ptr<DecodedImage> DecoderWic::decode(const std::filesystem::path& path,
                                                 std::string* error) {
    return decodeInternal(path, error, false);
}

std::shared_ptr<DecodedImage> DecoderWic::decodePage(const std::filesystem::path& path,
                                                     const uint32_t index, std::string* error) {
    return decodeInternal(path, error, false, index);
}

SequenceInfo DecoderWic::probeSequence(const std::filesystem::path& path) {
    SequenceInfo info;
    IWICImagingFactory* factory = wicFactoryForThisThread();
    if (!factory) return info;

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                  WICDecodeMetadataCacheOnDemand, &decoder))) {
        return info;
    }
    UINT frameCount = 0;
    if (FAILED(decoder->GetFrameCount(&frameCount)) || frameCount < 2) return info;

    GUID container{};
    if (FAILED(decoder->GetContainerFormat(&container))) return info;

    if (container == GUID_ContainerFormatGif) {
        info.kind = SequenceKind::Animation;
        info.frameCount = frameCount;
        ComPtr<IWICMetadataQueryReader> reader;
        if (SUCCEEDED(decoder->GetMetadataQueryReader(&reader))) {
            info.loopCount = readGifLoopCount(reader.Get());
        }
        return info;
    }

    // TIFF のページ、ICO のサイズ違い。素性の分からない多フレーム形式もこちらへ寄せる
    // (独立にデコードできると仮定するのが安全。アニメとして扱うと合成を誤る)
    info.kind = SequenceKind::Pages;
    info.frameCount = frameCount;
    if (container == GUID_ContainerFormatIco) {
        // 大きい順に並べ替え、各サイズを表示名にする(index 0 = 最大 = decode が返すもの)
        info.labels.reserve(frameCount);
        for (const UINT wicIndex : icoFrameOrder(decoder.Get(), frameCount)) {
            ComPtr<IWICBitmapFrameDecode> frame;
            UINT w = 0, h = 0;
            if (SUCCEEDED(decoder->GetFrame(wicIndex, &frame)) &&
                SUCCEEDED(frame->GetSize(&w, &h))) {
                info.labels.push_back(std::format("{} x {}", w, h));
            } else {
                info.labels.emplace_back();
            }
        }
    }
    return info;
}

bool DecoderWic::decodeAnimation(const std::filesystem::path& path, const AnimationLimits& limits,
                                 ImageSequence& out, std::string* error) {
    IWICImagingFactory* factory = wicFactoryForThisThread();
    if (!factory) {
        setError(error, "WICファクトリ生成");
        return false;
    }
    ComPtr<IWICBitmapDecoder> decoder;
    HRESULT hr = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                    WICDecodeMetadataCacheOnDemand, &decoder);
    if (FAILED(hr)) {
        setError(error, "デコーダ生成", hr);
        return false;
    }
    UINT frameCount = 0;
    if (FAILED(decoder->GetFrameCount(&frameCount)) || frameCount < 2) return false;
    if (frameCount > limits.maxFrames) {
        setError(error, std::format("フレーム数が多すぎる ({})", frameCount));
        out.truncated = true;
        return false;
    }

    // 論理画面の大きさ。取れなければ各フレームの矩形の和で代用する
    ComPtr<IWICMetadataQueryReader> fileReader;
    unsigned screenWidth = 0;
    unsigned screenHeight = 0;
    int loopCount = 1;
    if (SUCCEEDED(decoder->GetMetadataQueryReader(&fileReader))) {
        readMetadataUint(fileReader.Get(), L"/logscrdesc/Width", screenWidth);
        readMetadataUint(fileReader.Get(), L"/logscrdesc/Height", screenHeight);
        loopCount = readGifLoopCount(fileReader.Get());
    }

    // 各フレームの位置・大きさ・遅延・後始末を先に集める(画素はまだ読まない)
    struct FrameMeta {
        ComPtr<IWICBitmapFrameDecode> frame;
        unsigned left = 0;
        unsigned top = 0;
        UINT width = 0;
        UINT height = 0;
        uint32_t delayMs = 0;
        FrameDisposal disposal = FrameDisposal::None;
    };
    std::vector<FrameMeta> metas(frameCount);
    for (UINT i = 0; i < frameCount; ++i) {
        FrameMeta& meta = metas[i];
        if (FAILED(decoder->GetFrame(i, &meta.frame)) ||
            FAILED(meta.frame->GetSize(&meta.width, &meta.height))) {
            setError(error, std::format("フレーム取得 ({}/{})", i + 1, frameCount));
            return false;
        }
        ComPtr<IWICMetadataQueryReader> reader;
        if (SUCCEEDED(meta.frame->GetMetadataQueryReader(&reader))) {
            readMetadataUint(reader.Get(), L"/imgdesc/Left", meta.left);
            readMetadataUint(reader.Get(), L"/imgdesc/Top", meta.top);
            unsigned delay = 0;  // GIF の遅延は 1/100 秒単位
            if (readMetadataUint(reader.Get(), L"/grctlext/Delay", delay)) {
                meta.delayMs = static_cast<uint32_t>(delay) * 10;
            }
            unsigned disposal = 0;
            if (readMetadataUint(reader.Get(), L"/grctlext/Disposal", disposal)) {
                if (disposal == 2) meta.disposal = FrameDisposal::Background;
                else if (disposal == 3) meta.disposal = FrameDisposal::Previous;
            }
        }
        screenWidth = std::max<unsigned>(screenWidth, meta.left + meta.width);
        screenHeight = std::max<unsigned>(screenHeight, meta.top + meta.height);
    }
    if (screenWidth == 0 || screenHeight == 0) return false;
    if (screenWidth > kMaxDimension || screenHeight > kMaxDimension) {
        setError(error, std::format("論理画面が大きすぎる ({} x {})", screenWidth, screenHeight));
        return false;
    }

    // 展開後の総量は「論理画面 x フレーム数」。超えるなら静止画のまま扱う
    const size_t totalBytes =
        static_cast<size_t>(screenWidth) * screenHeight * 4 * frameCount;
    if (totalBytes > limits.maxBytes) {
        setError(error, std::format("展開に {} MB 必要", (totalBytes + (1 << 20) - 1) >> 20));
        out.truncated = true;
        return false;
    }

    AnimationCompositor compositor(screenWidth, screenHeight);
    out.kind = SequenceKind::Animation;
    out.loopCount = loopCount;
    out.frames.clear();
    out.frames.reserve(frameCount);
    for (UINT i = 0; i < frameCount; ++i) {
        const FrameMeta& meta = metas[i];
        std::shared_ptr<DecodedImage> sub = framePixels(factory, meta.frame.Get());
        if (!sub) {
            setError(error, std::format("フレーム展開 ({}/{})", i + 1, frameCount));
            return false;
        }
        // GIF の透明色は二値なので、事前乗算どうしの Over が「不透明な画素だけ置き換える」
        // という GIF 本来の重ね方と一致する
        std::shared_ptr<DecodedImage> composed =
            compositor.addFrame(*sub, static_cast<int32_t>(meta.left),
                                static_cast<int32_t>(meta.top), FrameBlend::Over, meta.disposal);
        if (!composed) {
            setError(error, std::format("フレーム合成 ({}/{})", i + 1, frameCount));
            return false;
        }
        out.frames.push_back(FrameEntry{std::move(composed), meta.delayMs, {}});
    }
    return true;
}

std::shared_ptr<DecodedImage> DecoderWic::decodeColorManaged(const std::filesystem::path& path,
                                                             std::string* error) {
    if (!colorManagement_) return nullptr;
    std::shared_ptr<DecodedImage> image = decodeInternal(path, error, true);
    // 変換が効かなかったなら差し替える意味がない(呼び出し側は最初の結果を使い続ける)
    if (!image || !image->colorConverted) return nullptr;
    return image;
}

std::shared_ptr<DecodedImage> DecoderWic::decodeInternal(const std::filesystem::path& path,
                                                        std::string* error,
                                                        const bool applyColorTransform,
                                                        const uint32_t frameIndex) {
    IWICImagingFactory* factory = wicFactoryForThisThread();
    if (!factory) {
        setError(error, "WICファクトリ生成");
        return nullptr;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    HRESULT hr = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                    WICDecodeMetadataCacheOnDemand, &decoder);
    if (FAILED(hr)) {
        // 対応するコーデックが無い(Windows 11 の HEIF/WebP/AVIF 拡張が未導入など)か、
        // ファイルが開けない・データが壊れている場合にここへ来る
        setError(error, "デコーダ生成(未対応形式かデータ破損)", hr);
        return nullptr;
    }

    // ICO だけは表示順(大きい順)と WIC のフレーム番号が食い違う。index 0 = 最大サイズ
    UINT frameCount = 0;
    UINT wicFrame = frameIndex;
    if (SUCCEEDED(decoder->GetFrameCount(&frameCount)) && frameCount > 1) {
        if (frameIndex >= frameCount) {
            setError(error, std::format("フレーム番号が範囲外 ({} / {})", frameIndex, frameCount));
            return nullptr;
        }
        wicFrame = resolveFrameIndex(decoder.Get(), frameCount, frameIndex);
    } else if (frameIndex != 0) {
        setError(error, std::format("フレーム番号が範囲外 ({} / 1)", frameIndex));
        return nullptr;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(wicFrame, &frame);
    if (FAILED(hr)) {
        setError(error, "フレーム取得", hr);
        return nullptr;
    }

    // EXIF 回転は「デコードが終わってから」自前で行う(core/exif の applyExifOrientation)。
    // IWICBitmapFlipRotator をコーデックへ直結すると、90/270 度回転では出力行ごとに
    // ソースへ細い矩形を要求するためコーデックが何千回もシークし直し、大きな JPEG
    // (iPhone の 12〜24MP 写真など)で事実上停止する
    const UINT16 orientation = readOrientation(frame.Get());

    ComPtr<IWICBitmapSource> source = frame;

    UINT width = 0;
    UINT height = 0;
    hr = source->GetSize(&width, &height);
    if (FAILED(hr)) {
        setError(error, "サイズ取得", hr);
        return nullptr;
    }
    if (width == 0 || height == 0) {
        setError(error, std::format("サイズが不正 ({} x {})", width, height));
        return nullptr;
    }

    // 巨大画像は上限に収まるよう縮小してから取り込む
    const UINT sourceWidth = width;
    const UINT sourceHeight = height;
    if (width > kMaxDimension || height > kMaxDimension) {
        const double scale = std::min(static_cast<double>(kMaxDimension) / width,
                                      static_cast<double>(kMaxDimension) / height);
        const UINT newWidth = std::max(1u, static_cast<UINT>(width * scale));
        const UINT newHeight = std::max(1u, static_cast<UINT>(height * scale));
        ComPtr<IWICBitmapScaler> scaler;
        hr = factory->CreateBitmapScaler(&scaler);
        if (SUCCEEDED(hr)) {
            hr = scaler->Initialize(source.Get(), newWidth, newHeight,
                                    WICBitmapInterpolationModeFant);
        }
        if (FAILED(hr)) {
            setError(error,
                     std::format("縮小 {} x {} → {} x {}", width, height, newWidth, newHeight),
                     hr);
            return nullptr;
        }
        source = scaler;
        width = newWidth;
        height = newHeight;
    }

    // 埋め込みプロファイル → sRGB。縮小した後に掛けて変換する画素数を減らす。
    // 最初の 1 回では変換せず、プロファイルの有無だけ見て colorPending を立てる
    // (変換は 24MP で 0.5 秒ほどかかるので、表示を待たせずに後から差し替える)
    bool colorConverted = false;
    bool colorPending = false;
    if (colorManagement_ && applyColorTransform) {
        if (ComPtr<IWICBitmapSource> transformed =
                colorTransformToSrgb(factory, frame.Get(), source.Get())) {
            source = transformed;
            colorConverted = true;
        }
    } else if (colorManagement_) {
        colorPending = inputColorContext(factory, frame.Get()) != nullptr;
    }

    // D2D が直接扱える 32bpp PBGRA (事前乗算) へ変換
    ComPtr<IWICFormatConverter> converter;
    hr = factory->CreateFormatConverter(&converter);
    if (SUCCEEDED(hr)) {
        hr = converter->Initialize(source.Get(), GUID_WICPixelFormat32bppPBGRA,
                                   WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeCustom);
    }
    if (FAILED(hr)) {
        setError(error, "PBGRA変換", hr);
        return nullptr;
    }

    const size_t byteSize = static_cast<size_t>(width) * height * 4;
    try {
        auto image = std::make_shared<DecodedImage>();
        image->width = width;
        image->height = height;
        image->colorConverted = colorConverted;
        image->colorPending = colorPending;
        if (width != sourceWidth || height != sourceHeight) {
            // 縮小した事実を残す。上書き保存の拒否とステータスバーの表示に使う
            image->sourceWidth = sourceWidth;
            image->sourceHeight = sourceHeight;
        }
        const UINT stride = width * 4;
        image->pixels.resize(byteSize);
        hr = converter->CopyPixels(nullptr, stride, static_cast<UINT>(image->pixels.size()),
                                   image->pixels.data());
        if (FAILED(hr)) {
            setError(error, std::format("ピクセル取得 ({} x {})", width, height), hr);
            return nullptr;
        }
        applyExifOrientation(*image, orientation);  // 失敗しても向きが元のまま残るだけ
        return image;
    } catch (const std::bad_alloc&) {
        setError(error, std::format("メモリ確保に失敗 ({} x {}, {} MB)", width, height,
                                    byteSize >> 20));
        return nullptr;
    }
}

} // namespace blinker
