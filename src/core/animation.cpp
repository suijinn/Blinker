#include "core/animation.h"

#include <algorithm>

namespace blinker {

AnimationOptions animationOptionsFromConfig(const Config& config) {
    const AnimationOptions defaults;
    AnimationOptions options;
    options.autoplay = config.getBool("animation", "autoplay", defaults.autoplay);
    options.loopForever = config.getBool("animation", "loop", defaults.loopForever);
    options.minDelayMs = static_cast<uint32_t>(
        std::clamp(config.getInt("animation", "min_delay_ms",
                                 static_cast<int>(defaults.minDelayMs)),
                   kMinAnimationDelayMs, kMaxAnimationDelayMs));
    options.defaultDelayMs = static_cast<uint32_t>(
        std::clamp(config.getInt("animation", "default_delay_ms",
                                 static_cast<int>(defaults.defaultDelayMs)),
                   1, kMaxAnimationDelayMs));
    return options;
}

AnimationLimits animationLimitsFromConfig(const Config& config) {
    const AnimationLimits defaults;
    const int memoryMB =
        std::clamp(config.getInt("animation", "max_memory_mb",
                                 static_cast<int>(defaults.maxBytes >> 20)),
                   kMinAnimationMemoryMB, kMaxAnimationMemoryMB);
    const int frames =
        std::clamp(config.getInt("animation", "max_frames",
                                 static_cast<int>(defaults.maxFrames)),
                   kMinAnimationFrames, kMaxAnimationFrames);
    return AnimationLimits{static_cast<size_t>(memoryMB) << 20, static_cast<uint32_t>(frames)};
}

uint32_t normalizedDelayMs(const uint32_t rawMs, const uint32_t minMs, const uint32_t defaultMs) {
    if (rawMs < minMs) return std::max(1u, defaultMs);
    return std::max(1u, rawMs);
}

bool advanceFrame(PlaybackState& state, const size_t frameCount, const int loopCount) {
    if (frameCount < 2) {
        state.playing = false;
        return false;
    }
    if (state.index + 1 < frameCount) {
        ++state.index;
        return true;
    }
    // 末尾。1 周し終えたので、繰り返し回数に達していなければ先頭へ戻る
    ++state.loopsDone;
    if (loopCount > 0 && state.loopsDone >= loopCount) {
        state.playing = false;
        return false;  // 最後のフレームを表示したまま止まる
    }
    state.index = 0;
    return true;
}

AnimationCompositor::AnimationCompositor(const uint32_t width, const uint32_t height)
    : width_(width), height_(height) {
    if (width_ == 0 || height_ == 0) return;
    canvas_.assign(static_cast<size_t>(width_) * height_ * 4, 0);  // 透明で初期化
}

std::shared_ptr<DecodedImage> AnimationCompositor::addFrame(const DecodedImage& sub,
                                                            const int32_t left, const int32_t top,
                                                            const FrameBlend blend,
                                                            const FrameDisposal disposal) {
    if (canvas_.empty()) return nullptr;

    // Previous はこのフレームを描く前の状態へ戻すので、描く前に退避しておく
    if (disposal == FrameDisposal::Previous) saved_ = canvas_;

    // キャンバスからはみ出す分を切り捨てた、実際に触る範囲を求める
    const int64_t x0 = std::max<int64_t>(left, 0);
    const int64_t y0 = std::max<int64_t>(top, 0);
    const int64_t x1 = std::min<int64_t>(static_cast<int64_t>(left) + sub.width, width_);
    const int64_t y1 = std::min<int64_t>(static_cast<int64_t>(top) + sub.height, height_);

    for (int64_t y = y0; y < y1; ++y) {
        const size_t srcRow = static_cast<size_t>(y - top) * sub.width * 4;
        const size_t dstRow = static_cast<size_t>(y) * width_ * 4;
        for (int64_t x = x0; x < x1; ++x) {
            const uint8_t* src = sub.pixels.data() + srcRow + static_cast<size_t>(x - left) * 4;
            uint8_t* dst = canvas_.data() + dstRow + static_cast<size_t>(x) * 4;
            if (blend == FrameBlend::Source) {
                dst[0] = src[0];
                dst[1] = src[1];
                dst[2] = src[2];
                dst[3] = src[3];
                continue;
            }
            // 事前乗算どうしの Over: dst = src + dst * (1 - srcA)
            const uint32_t inv = 255u - src[3];
            for (int c = 0; c < 4; ++c) {
                dst[c] = static_cast<uint8_t>(
                    std::min<uint32_t>(255, src[c] + (dst[c] * inv + 127) / 255));
            }
        }
    }

    auto frame = std::make_shared<DecodedImage>();
    frame->width = width_;
    frame->height = height_;
    frame->pixels = canvas_;  // この時点の見た目を 1 枚として切り出す

    switch (disposal) {
    case FrameDisposal::Background:
        // 次のフレームのために、このフレームの矩形を透明へ戻す
        for (int64_t y = y0; y < y1; ++y) {
            uint8_t* row = canvas_.data() + static_cast<size_t>(y) * width_ * 4;
            std::fill_n(row + static_cast<size_t>(x0) * 4, static_cast<size_t>(x1 - x0) * 4,
                        uint8_t{0});
        }
        break;
    case FrameDisposal::Previous:
        canvas_ = saved_;
        break;
    case FrameDisposal::None:
        break;
    }
    return frame;
}

} // namespace blinker
