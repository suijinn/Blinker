#include "win/decoder_wic.h"

#include <windows.h>

#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <format>
#include <new>
#include <string_view>
#include <vector>

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

} // namespace

std::shared_ptr<DecodedImage> DecoderWic::decode(const std::filesystem::path& path,
                                                 std::string* error) {
    return decodeInternal(path, error, false);
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
                                                        const bool applyColorTransform) {
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

    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
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
