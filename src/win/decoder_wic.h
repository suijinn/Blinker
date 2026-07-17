#pragma once

#include "platform/decoder.h"

namespace blinker {

// WIC による画像デコーダ。スレッドごとに COM/ファクトリを初期化するため、
// どのスレッドから呼んでもよい。
class DecoderWic final : public IImageDecoder {
public:
    std::shared_ptr<DecodedImage> decode(const std::filesystem::path& path) override;
};

} // namespace blinker
