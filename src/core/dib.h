#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "platform/decoder.h"

/**
 * @file dib.h
 * @brief パックド DIB から DecodedImage への変換(クリップボード読み取り用)。
 */

namespace blinker {

/**
 * @brief パックド DIB を 32bpp PBGRA の DecodedImage へ変換する。
 *
 * BITMAPINFOHEADER/V4/V5 + パレット/マスク + ピクセルの連続バイト列を受け取り、
 * アルファ事前乗算済みの画像へ変換する。クリップボードの CF_DIB / CF_DIBV5 の
 * 読み取りに使う。対応: 8/16/24/32bpp、BI_RGB / BI_BITFIELDS、
 * トップダウン / ボトムアップ。
 *
 * OS ヘッダに依存しない純粋関数(単体テスト対象)。
 *
 * @param[in] data DIB の先頭を指すバッファ。
 * @param[in] size バッファのバイト数。
 * @return 変換された画像。非対応形式・不正データの場合は nullptr。
 */
std::shared_ptr<DecodedImage> imageFromDib(const uint8_t* data, size_t size);

} // namespace blinker
