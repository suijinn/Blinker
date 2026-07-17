#include "win/encoder_wic.h"

#include <windows.h>

#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cwctype>
#include <optional>
#include <string>
#include <vector>

#include "core/pixel_convert.h"
#include "win/wic_factory.h"

namespace blinker {
namespace {

using Microsoft::WRL::ComPtr;

struct FormatChoice {
    GUID container;
    WICPixelFormatGUID pixelFormat;  // WIC へ渡す入力ピクセル形式
    bool keepAlpha;
    bool isJpeg;
};

std::optional<FormatChoice> choiceFromExtension(std::wstring ext) {
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    if (ext == L".png") {
        return FormatChoice{GUID_ContainerFormatPng, GUID_WICPixelFormat32bppBGRA, true, false};
    }
    if (ext == L".jpg" || ext == L".jpeg" || ext == L".jpe" || ext == L".jfif") {
        return FormatChoice{GUID_ContainerFormatJpeg, GUID_WICPixelFormat24bppBGR, false, true};
    }
    if (ext == L".bmp" || ext == L".dib") {
        return FormatChoice{GUID_ContainerFormatBmp, GUID_WICPixelFormat24bppBGR, false, false};
    }
    return std::nullopt;
}

} // namespace

bool EncoderWic::encode(const DecodedImage& image, const std::filesystem::path& path) {
    if (image.width == 0 || image.height == 0) return false;
    IWICImagingFactory* factory = wicFactoryForThisThread();
    if (!factory) return false;
    const auto choice = choiceFromExtension(path.extension().wstring());
    if (!choice) return false;

    // PBGRA からエンコーダ入力形式へ変換(PNG: 逆乗算 BGRA、JPEG/BMP: 白合成 BGR)
    const std::vector<uint8_t> pixels =
        choice->keepAlpha ? toStraightBGRA(image) : toOpaqueBGR(image);
    const UINT stride = image.width * (choice->keepAlpha ? 4u : 3u);

    ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) {
        return false;
    }

    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(factory->CreateEncoder(choice->container, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) {
        return false;
    }

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    if (FAILED(encoder->CreateNewFrame(&frame, &props))) return false;
    if (choice->isJpeg && props) {
        wchar_t optionName[] = L"ImageQuality";
        PROPBAG2 option{};
        option.pstrName = optionName;
        VARIANT value{};
        value.vt = VT_R4;
        value.fltVal = 0.9f;
        props->Write(1, &option, &value);  // 失敗しても既定品質で続行
    }
    if (FAILED(frame->Initialize(props.Get())) ||
        FAILED(frame->SetSize(image.width, image.height))) {
        return false;
    }
    WICPixelFormatGUID actualFormat = choice->pixelFormat;
    if (FAILED(frame->SetPixelFormat(&actualFormat))) return false;

    ComPtr<IWICBitmap> bitmap;
    if (FAILED(factory->CreateBitmapFromMemory(
            image.width, image.height, choice->pixelFormat, stride,
            static_cast<UINT>(pixels.size()), const_cast<BYTE*>(pixels.data()), &bitmap))) {
        return false;
    }

    // エンコーダが入力形式をそのまま受けない場合は WIC に変換させる
    ComPtr<IWICBitmapSource> source = bitmap;
    if (actualFormat != choice->pixelFormat) {
        ComPtr<IWICBitmapSource> converted;
        if (FAILED(WICConvertBitmapSource(actualFormat, source.Get(), &converted))) return false;
        source = converted;
    }
    if (FAILED(frame->WriteSource(source.Get(), nullptr))) return false;
    return SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
}

} // namespace blinker
