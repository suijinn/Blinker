#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "core/config.h"
#include "platform/decoder.h"

/**
 * @file animation.h
 * @brief アニメーションの合成・再生位置・遅延時間(プラットフォーム非依存の純粋ロジック)。
 */

namespace blinker {

/**
 * @brief フレームを表示し終えた後の後始末。
 *
 * GIF の Graphic Control Extension の Disposal Method に対応する。
 */
enum class FrameDisposal {
    None,        ///< そのまま残す(次のフレームを重ねる)
    Background,  ///< そのフレームの矩形を透明に戻す
    Previous,    ///< そのフレームを描く前のキャンバスへ戻す
};

/**
 * @brief フレームをキャンバスへ描き込む方法。
 */
enum class FrameBlend {
    Over,    ///< アルファ合成して重ねる
    Source,  ///< 矩形をそのまま置き換える(透明部分も透明で上書きする)
};

/**
 * @brief アニメーションの再生に関する設定([animation] セクション)。
 */
struct AnimationOptions {
    bool autoplay = true;      ///< 開いたときに自動で再生を始めるか
    bool loopForever = true;   ///< ファイルのループ回数を無視して無限に繰り返すか
    /// これ未満の遅延指定を defaultDelayMs へ読み替える(0/10ms 指定への対処)
    uint32_t minDelayMs = 20;
    uint32_t defaultDelayMs = 100;  ///< 遅延指定が無い・小さすぎるときに使う時間 (ms)
};

/// @brief animationOptionsFromConfig / animationLimitsFromConfig が受け付ける値の範囲。
constexpr int kMinAnimationDelayMs = 0;      ///< min_delay_ms の下限
constexpr int kMaxAnimationDelayMs = 5000;   ///< min_delay_ms / default_delay_ms の上限
constexpr int kMinAnimationMemoryMB = 16;    ///< max_memory_mb の下限
constexpr int kMaxAnimationMemoryMB = 4096;  ///< max_memory_mb の上限
constexpr int kMinAnimationFrames = 2;       ///< max_frames の下限
constexpr int kMaxAnimationFrames = 100000;  ///< max_frames の上限

/**
 * @brief blinker.ini の `[animation]` から再生設定を読む。
 * @param[in] config 読み込み済みの設定。
 * @return 適用する設定。キーが無ければ AnimationOptions の既定値。
 */
AnimationOptions animationOptionsFromConfig(const Config& config);

/**
 * @brief blinker.ini の `[animation]` から展開の上限を読む。
 * @param[in] config 読み込み済みの設定。
 * @return 適用する上限。キーが無ければ AnimationLimits の既定値。
 */
AnimationLimits animationLimitsFromConfig(const Config& config);

/**
 * @brief フレームの遅延指定を実際に待つ時間へ正規化する。
 *
 * GIF は 0 や 10ms の遅延指定を持つものが多く、そのまま従うと数百 fps になる。
 * 主要ブラウザと同じく、小さすぎる値は既定値へ読み替える。
 *
 * @param[in] rawMs      ファイルに書かれていた遅延時間 (ms)。
 * @param[in] minMs      これ未満なら defaultMs を使う閾値 (ms)。
 * @param[in] defaultMs  読み替え先の時間 (ms)。
 * @return 実際に待つ時間 (ms)。1 以上になる。
 */
uint32_t normalizedDelayMs(uint32_t rawMs, uint32_t minMs, uint32_t defaultMs);

/**
 * @brief アニメーションの再生位置。
 */
struct PlaybackState {
    size_t index = 0;    ///< 表示中のフレーム番号(0 起点)
    bool playing = false;  ///< 自動再生中か
    int loopsDone = 0;   ///< 末尾まで再生し終えた回数
};

/**
 * @brief 再生位置を次のフレームへ進める。
 *
 * 末尾に達したら loopCount に従って先頭へ戻すか、再生を止める。
 * 時計に依存しないので単体テストできる。
 *
 * @param[in,out] state      更新する再生位置。
 * @param[in]     frameCount フレーム数。
 * @param[in]     loopCount  繰り返し回数。0 なら無限に繰り返す。
 * @return 表示するフレームが変わったら true(呼び出し側は再描画する)。
 * @note frameCount が 2 未満なら再生を止めて false を返す。
 */
bool advanceFrame(PlaybackState& state, size_t frameCount, int loopCount);

/**
 * @brief アニメーションのフレームを 1 枚ずつ全画面のフレームへ合成する。
 *
 * GIF のフレームは「部分矩形 + 後始末の指定」で表されるため、表示できる 1 枚に
 * するには前のフレームの上へ順に重ねる必要がある。この合成は画素演算だけなので
 * core に置いて単体テスト対象にしてある。
 *
 * @note 画素は 32bpp PBGRA(事前乗算)。GIF の Background は**透明**で塗る
 *       (背景色ではない。主要ブラウザと同じ挙動)。
 */
class AnimationCompositor {
public:
    /**
     * @brief 透明で初期化したキャンバスを作る。
     * @param[in] width  キャンバスの幅(ピクセル)。
     * @param[in] height キャンバスの高さ(ピクセル)。
     */
    AnimationCompositor(uint32_t width, uint32_t height);

    /**
     * @brief フレームを 1 枚重ねて、表示できる全画面のフレームを返す。
     *
     * 手順は「Previous 用の退避 → 描き込み → キャンバスの複製を返す → 後始末」。
     * disposal は**返したフレームを表示し終えた後**に適用されるので、次の addFrame へ
     * 引き継がれる。
     *
     * @param[in] sub      重ねる部分画像(32bpp PBGRA)。
     * @param[in] left     キャンバス上の配置位置 X。
     * @param[in] top      キャンバス上の配置位置 Y。
     * @param[in] blend    描き込み方。
     * @param[in] disposal このフレームを表示し終えた後の後始末。
     * @return 合成済みの全画面フレーム。キャンバスが空(幅か高さが 0)なら nullptr。
     * @note sub がキャンバスからはみ出す分は切り捨てる。
     */
    std::shared_ptr<DecodedImage> addFrame(const DecodedImage& sub, int32_t left, int32_t top,
                                           FrameBlend blend, FrameDisposal disposal);

private:
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    std::vector<uint8_t> canvas_;  ///< 現在のキャンバス(32bpp PBGRA)
    std::vector<uint8_t> saved_;   ///< FrameDisposal::Previous 用の退避
};

} // namespace blinker
