#include "win/decoder_wic.h"

#include <windows.h>

#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <format>
#include <new>
#include <string_view>

#include "core/exif.h"
#include "win/wic_factory.h"

namespace blinker {
namespace {

using Microsoft::WRL::ComPtr;

// D2D の ID2D1Bitmap が確実に扱える上限に収める
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

} // namespace

std::shared_ptr<DecodedImage> DecoderWic::decode(const std::filesystem::path& path,
                                                 std::string* error) {
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

    // 巨大画像は描画側の上限に収まるよう縮小してから取り込む
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
