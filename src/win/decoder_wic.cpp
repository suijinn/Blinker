#include "win/decoder_wic.h"

#include <windows.h>

#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <new>

namespace blinker {
namespace {

using Microsoft::WRL::ComPtr;

// D2D の ID2D1Bitmap が確実に扱える上限に収める
constexpr UINT kMaxDimension = 16384;

// スレッドごとに COM とファクトリを初期化して使い回す
IWICImagingFactory* factoryForThisThread() {
    thread_local struct ThreadState {
        bool comInitialized = false;
        ComPtr<IWICImagingFactory> factory;

        ThreadState() {
            // RPC_E_CHANGED_MODE(既に別モードで初期化済み)でも COM は利用できる
            comInitialized = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
            CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                             IID_PPV_ARGS(&factory));
        }
        ~ThreadState() {
            factory.Reset();
            if (comInitialized) CoUninitialize();
        }
    } state;
    return state.factory.Get();
}

// EXIF Orientation (1-8) → WIC の変換オプション
WICBitmapTransformOptions transformFromOrientation(UINT16 orientation) {
    switch (orientation) {
    case 2: return WICBitmapTransformFlipHorizontal;
    case 3: return WICBitmapTransformRotate180;
    case 4: return WICBitmapTransformFlipVertical;
    case 5:
        return static_cast<WICBitmapTransformOptions>(WICBitmapTransformRotate90 |
                                                      WICBitmapTransformFlipHorizontal);
    case 6: return WICBitmapTransformRotate90;
    case 7:
        return static_cast<WICBitmapTransformOptions>(WICBitmapTransformRotate270 |
                                                      WICBitmapTransformFlipHorizontal);
    case 8: return WICBitmapTransformRotate270;
    default: return WICBitmapTransformRotate0;
    }
}

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

} // namespace

std::shared_ptr<DecodedImage> DecoderWic::decode(const std::filesystem::path& path) {
    IWICImagingFactory* factory = factoryForThisThread();
    if (!factory) return nullptr;

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                  WICDecodeMetadataCacheOnDemand, &decoder))) {
        return nullptr;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) return nullptr;

    ComPtr<IWICBitmapSource> source = frame;

    // EXIF 回転を適用(スマホ写真などを正しい向きで表示する)
    const UINT16 orientation = readOrientation(frame.Get());
    if (orientation > 1) {
        ComPtr<IWICBitmapFlipRotator> rotator;
        if (SUCCEEDED(factory->CreateBitmapFlipRotator(&rotator)) &&
            SUCCEEDED(rotator->Initialize(source.Get(), transformFromOrientation(orientation)))) {
            source = rotator;
        }
    }

    UINT width = 0;
    UINT height = 0;
    if (FAILED(source->GetSize(&width, &height)) || width == 0 || height == 0) return nullptr;

    // 巨大画像は描画側の上限に収まるよう縮小してから取り込む
    if (width > kMaxDimension || height > kMaxDimension) {
        const double scale = std::min(static_cast<double>(kMaxDimension) / width,
                                      static_cast<double>(kMaxDimension) / height);
        const UINT newWidth = std::max(1u, static_cast<UINT>(width * scale));
        const UINT newHeight = std::max(1u, static_cast<UINT>(height * scale));
        ComPtr<IWICBitmapScaler> scaler;
        if (FAILED(factory->CreateBitmapScaler(&scaler)) ||
            FAILED(scaler->Initialize(source.Get(), newWidth, newHeight,
                                      WICBitmapInterpolationModeFant))) {
            return nullptr;
        }
        source = scaler;
        width = newWidth;
        height = newHeight;
    }

    // D2D が直接扱える 32bpp PBGRA (事前乗算) へ変換
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(source.Get(), GUID_WICPixelFormat32bppPBGRA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom))) {
        return nullptr;
    }

    try {
        auto image = std::make_shared<DecodedImage>();
        image->width = width;
        image->height = height;
        const UINT stride = width * 4;
        image->pixels.resize(static_cast<size_t>(stride) * height);
        if (FAILED(converter->CopyPixels(nullptr, stride,
                                         static_cast<UINT>(image->pixels.size()),
                                         image->pixels.data()))) {
            return nullptr;
        }
        return image;
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
}

} // namespace blinker
