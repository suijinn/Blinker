#include "core/image_scale.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <new>
#include <vector>

namespace blinker {
namespace {

/// 重みの固定小数の 1.0。1 画素あたりの合計は 255 * kWeightOne = 約 418 万で int32 に収まる
constexpr int32_t kWeightOne = 16384;

/**
 * @brief 1 軸ぶんのリサンプル重み表。
 *
 * 出力画素 i は入力画素 [first[i], first[i] + taps) に weights[i * taps + j] を掛けた
 * 加重平均になる。タップ数は出力画素ごとに ±1 変わりうるが、使わない分を 0 で
 * 埋めた固定長にして添字計算を単純にしている。
 */
struct AxisWeights {
    uint32_t taps = 0;              ///< 1 出力画素あたりのタップ数(weights の行幅)
    std::vector<uint32_t> first;    ///< 出力画素ごとの先頭入力画素
    std::vector<int32_t> weights;   ///< 重み(行ごとの合計が kWeightOne)
};

// 三角(テント)フィルタの重み表を作る。半径を max(1, 1/倍率) にすると、縮小では
// 入力画素をまんべんなく拾う面積平均相当に、拡大ではバイリニアになる
AxisWeights buildAxisWeights(const uint32_t srcN, const uint32_t dstN) {
    const double scale = static_cast<double>(dstN) / static_cast<double>(srcN);
    const double radius = scale < 1.0 ? 1.0 / scale : 1.0;
    AxisWeights axis;
    // タップ数は入力の幅を超えないよう抑える。読み出しは [first, first + taps) の
    // 固定長なので、この上限と下の first のクランプで範囲外アクセスを防いでいる
    axis.taps = std::min(static_cast<uint32_t>(std::ceil(radius * 2.0)) + 2, srcN);
    axis.first.resize(dstN);
    axis.weights.assign(static_cast<size_t>(dstN) * axis.taps, 0);

    std::vector<double> row(axis.taps);
    for (uint32_t i = 0; i < dstN; ++i) {
        // 出力画素の中心を入力座標へ写した位置。両端の 0.5 は画素中心のぶん
        const double center = (static_cast<double>(i) + 0.5) / scale - 0.5;
        const int lo = static_cast<int>(std::ceil(center - radius));
        const int hi = static_cast<int>(std::floor(center + radius));
        const int maxIndex = static_cast<int>(srcN) - 1;
        // 画像の外へはみ出した分は端の画素へ畳み込む(端が暗くならないように)
        const uint32_t first =
            static_cast<uint32_t>(std::clamp(lo, 0, static_cast<int>(srcN - axis.taps)));
        axis.first[i] = first;

        std::fill(row.begin(), row.end(), 0.0);
        double sum = 0;
        for (int k = lo; k <= hi; ++k) {
            const double distance = std::abs((static_cast<double>(k) - center) / radius);
            if (distance >= 1.0) continue;
            const double weight = 1.0 - distance;
            const uint32_t slot = static_cast<uint32_t>(std::clamp(k, 0, maxIndex)) - first;
            if (slot >= axis.taps) continue;  // 念のための保険(通常は起きない)
            row[slot] += weight;
            sum += weight;
        }
        if (sum <= 0) {  // 端の 1 画素だけを見る退化ケース
            row[0] = 1.0;
            sum = 1.0;
        }
        // 丸め誤差で合計が kWeightOne からずれると明るさが変わるため、最後の非ゼロで帳尻を合わせる
        int32_t assigned = 0;
        uint32_t lastSlot = 0;
        for (uint32_t j = 0; j < axis.taps; ++j) {
            const int32_t w = static_cast<int32_t>(std::lround(row[j] / sum * kWeightOne));
            axis.weights[static_cast<size_t>(i) * axis.taps + j] = w;
            assigned += w;
            if (w != 0) lastSlot = j;
        }
        axis.weights[static_cast<size_t>(i) * axis.taps + lastSlot] += kWeightOne - assigned;
    }
    return axis;
}

// 水平方向のリサンプル。src (srcW x height) → dst (dstW x height)
void resampleHorizontal(const uint8_t* src, const uint32_t srcW, const uint32_t height,
                        uint8_t* dst, const uint32_t dstW, const AxisWeights& axis) {
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* srcRow = src + static_cast<size_t>(y) * srcW * 4;
        uint8_t* dstRow = dst + static_cast<size_t>(y) * dstW * 4;
        for (uint32_t x = 0; x < dstW; ++x) {
            const int32_t* w = axis.weights.data() + static_cast<size_t>(x) * axis.taps;
            const uint8_t* p = srcRow + static_cast<size_t>(axis.first[x]) * 4;
            int32_t sum[4] = {0, 0, 0, 0};
            for (uint32_t j = 0; j < axis.taps; ++j, p += 4) {
                sum[0] += w[j] * p[0];
                sum[1] += w[j] * p[1];
                sum[2] += w[j] * p[2];
                sum[3] += w[j] * p[3];
            }
            uint8_t* d = dstRow + static_cast<size_t>(x) * 4;
            for (int c = 0; c < 4; ++c) {
                d[c] = static_cast<uint8_t>(
                    std::clamp((sum[c] + kWeightOne / 2) / kWeightOne, 0, 255));
            }
        }
    }
}

// 垂直方向のリサンプル。src (width x 任意の高さ) → dst (width x dstH)。
// 入力の高さは重み表(axis)に織り込まれているのでここでは要らない
void resampleVertical(const uint8_t* src, const uint32_t width, uint8_t* dst,
                      const uint32_t dstH, const AxisWeights& axis) {
    for (uint32_t y = 0; y < dstH; ++y) {
        const int32_t* w = axis.weights.data() + static_cast<size_t>(y) * axis.taps;
        uint8_t* dstRow = dst + static_cast<size_t>(y) * width * 4;
        for (uint32_t x = 0; x < width; ++x) {
            const uint8_t* p =
                src + (static_cast<size_t>(axis.first[y]) * width + x) * 4;
            int32_t sum[4] = {0, 0, 0, 0};
            for (uint32_t j = 0; j < axis.taps; ++j, p += static_cast<size_t>(width) * 4) {
                sum[0] += w[j] * p[0];
                sum[1] += w[j] * p[1];
                sum[2] += w[j] * p[2];
                sum[3] += w[j] * p[3];
            }
            uint8_t* d = dstRow + static_cast<size_t>(x) * 4;
            for (int c = 0; c < 4; ++c) {
                d[c] = static_cast<uint8_t>(
                    std::clamp((sum[c] + kWeightOne / 2) / kWeightOne, 0, 255));
            }
        }
    }
}

} // namespace

std::shared_ptr<DecodedImage> downscaleToFit(const DecodedImage& image,
                                             const uint32_t maxDimension) {
    if (maxDimension == 0) return nullptr;
    const uint32_t srcW = image.width;
    const uint32_t srcH = image.height;
    if (srcW == 0 || srcH == 0) return nullptr;
    if (static_cast<size_t>(srcW) * srcH * 4 > image.pixels.size()) return nullptr;
    if (srcW <= maxDimension && srcH <= maxDimension) return nullptr;  // 縮小不要

    // 長い辺を maxDimension に合わせる(短い辺は比率のまま)
    const double scale = std::min(static_cast<double>(maxDimension) / srcW,
                                  static_cast<double>(maxDimension) / srcH);
    const uint32_t dstW =
        std::clamp(static_cast<uint32_t>(static_cast<double>(srcW) * scale), 1u, maxDimension);
    const uint32_t dstH =
        std::clamp(static_cast<uint32_t>(static_cast<double>(srcH) * scale), 1u, maxDimension);

    std::shared_ptr<DecodedImage> out;
    try {
        out = std::make_shared<DecodedImage>();
        out->pixels.resize(static_cast<size_t>(dstW) * dstH * 4);
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
    out->width = dstW;
    out->height = dstH;
    // 既に縮小済みの画像を更に縮めることもあるので、元の大きさは引き継ぐ
    out->sourceWidth = image.sourceWidth != 0 ? image.sourceWidth : srcW;
    out->sourceHeight = image.sourceHeight != 0 ? image.sourceHeight : srcH;

    const uint8_t* src = image.pixels.data();
    uint8_t* dst = out->pixels.data();
    for (uint32_t y = 0; y < dstH; ++y) {
        // 出力画素が覆う入力の行範囲 [sy0, sy1)。丸めで空にならないよう最低 1 行は取る
        const uint32_t sy0 = static_cast<uint32_t>(static_cast<uint64_t>(y) * srcH / dstH);
        const uint32_t sy1 =
            std::max(sy0 + 1, static_cast<uint32_t>(static_cast<uint64_t>(y + 1) * srcH / dstH));
        for (uint32_t x = 0; x < dstW; ++x) {
            const uint32_t sx0 = static_cast<uint32_t>(static_cast<uint64_t>(x) * srcW / dstW);
            const uint32_t sx1 = std::max(
                sx0 + 1, static_cast<uint32_t>(static_cast<uint64_t>(x + 1) * srcW / dstW));
            // 縮小率が大きいと画素数が数億に達するので合計は 64bit で持つ
            uint64_t sum[4] = {0, 0, 0, 0};
            for (uint32_t sy = sy0; sy < sy1; ++sy) {
                const uint8_t* p = src + (static_cast<size_t>(sy) * srcW + sx0) * 4;
                for (uint32_t sx = sx0; sx < sx1; ++sx, p += 4) {
                    sum[0] += p[0];
                    sum[1] += p[1];
                    sum[2] += p[2];
                    sum[3] += p[3];
                }
            }
            const uint64_t count = static_cast<uint64_t>(sy1 - sy0) * (sx1 - sx0);
            uint8_t* d = dst + (static_cast<size_t>(y) * dstW + x) * 4;
            for (int c = 0; c < 4; ++c) d[c] = static_cast<uint8_t>(sum[c] / count);
        }
    }
    return out;
}

std::shared_ptr<DecodedImage> resizeImage(const DecodedImage& src, const uint32_t width,
                                          const uint32_t height) {
    const uint32_t srcW = src.width;
    const uint32_t srcH = src.height;
    if (width == 0 || height == 0 || srcW == 0 || srcH == 0) return nullptr;
    if (width > kMaxResizeDimension || height > kMaxResizeDimension) return nullptr;
    if (static_cast<size_t>(srcW) * srcH * 4 > src.pixels.size()) return nullptr;

    // 水平 → 垂直の 2 パス。中間バッファは (width x srcH)
    std::vector<uint8_t> intermediate;
    std::shared_ptr<DecodedImage> out;
    try {
        intermediate.resize(static_cast<size_t>(width) * srcH * 4);
        out = std::make_shared<DecodedImage>();
        out->pixels.resize(static_cast<size_t>(width) * height * 4);
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
    out->width = width;
    out->height = height;
    // 取り込み時に縮小された画像は、リサイズしても元ファイルの画素を持たないままなので
    // 上書き保存を拒む印を引き継ぐ。等倍で取り込んだ画像は 0 のまま(通常の保存ができる)
    out->sourceWidth = src.sourceWidth;
    out->sourceHeight = src.sourceHeight;
    out->colorConverted = src.colorConverted;

    resampleHorizontal(src.pixels.data(), srcW, srcH, intermediate.data(), width,
                       buildAxisWeights(srcW, width));
    resampleVertical(intermediate.data(), width, out->pixels.data(), height,
                     buildAxisWeights(srcH, height));
    return out;
}

} // namespace blinker
