#pragma once

#include <filesystem>

#include "platform/decoder.h"

namespace blinker {

// 画像エンコーダのプラットフォーム抽象。Windows 実装は WIC (encoder_wic)。
// UI スレッドからのみ呼ばれる(保存は同期処理)。
class IImageEncoder {
public:
    virtual ~IImageEncoder() = default;

    // image (32bpp PBGRA) を path へ保存する。形式は拡張子で決まる
    // (.png / .jpg / .jpeg / .bmp)。非対応の拡張子・書き込み失敗は false
    virtual bool encode(const DecodedImage& image, const std::filesystem::path& path) = 0;
};

} // namespace blinker
