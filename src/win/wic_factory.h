#pragma once

struct IWICImagingFactory;

namespace blinker {

// スレッドごとに COM と WIC ファクトリを初期化して使い回す。
// decoder_wic (ワーカースレッド) と encoder_wic (UI スレッド) で共用。失敗時 nullptr
IWICImagingFactory* wicFactoryForThisThread();

} // namespace blinker
