#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "platform/decoder.h"

namespace blinker {

// パックド DIB (BITMAPINFOHEADER/V4/V5 + パレット/マスク + ピクセル) を
// 32bpp PBGRA (事前乗算) の DecodedImage へ変換する。クリップボードの
// CF_DIB / CF_DIBV5 の読み取りに使う。対応: 8/16/24/32bpp、BI_RGB / BI_BITFIELDS、
// トップダウン / ボトムアップ。非対応形式・不正データは nullptr。
// OS ヘッダに依存しない純粋関数(単体テスト対象)。
std::shared_ptr<DecodedImage> imageFromDib(const uint8_t* data, size_t size);

} // namespace blinker
