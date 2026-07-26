#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "platform/decoder.h"
#include "platform/ocr.h"

/**
 * @file ocr_service.h
 * @brief 文字認識 (OCR) の非同期実行。
 */

namespace blinker {

/**
 * @brief 文字認識を UI スレッドの外で走らせる。
 *
 * ワーカースレッド 1 本が認識を担う(ImageCache と同じ形)。setOnCompleted で
 * 登録したコールバックはワーカースレッド上で呼ばれるため、受け側(win/sdl 層)は
 * UI スレッドへの通知 (PostMessage 等) に変換すること。
 *
 * 認識は数百ミリ秒から数秒かかる。UI スレッドで直接呼ぶと固まるので、
 * この経路を通すこと。
 */
class OcrService {
public:
    /// @brief 1 件の認識結果。
    struct Completed {
        uint64_t generation = 0;  ///< 対応する request の戻り値
        bool ok = false;          ///< 認識できたか
        OcrResult result;         ///< 認識結果(ok が false なら無意味)
        std::string error;        ///< 失敗理由(ok が true なら空)
    };

    /**
     * @brief サービスを構築し、ワーカースレッドを起動する。
     * @param[in] engine 認識に使う実装。本オブジェクトより長生きすること。
     */
    explicit OcrService(IOcrEngine& engine);

    /// @brief ワーカースレッドを停止して待ち合わせる(実行中の認識の完了を待つ)。
    ~OcrService();

    OcrService(const OcrService&) = delete;
    OcrService& operator=(const OcrService&) = delete;

    /**
     * @brief 認識を予約する。
     *
     * 未処理の予約が残っていれば置き換える(最後の 1 件だけが実行される)。
     * 既に走り出した認識は止められないため、その結果は古い generation を持って
     * 完了する。呼び出し側は takeResult の generation で捨てること。
     *
     * @param[in] image 認識対象の画像。ワーカースレッドと共有するため、
     *                  投入後に書き換えてはならない。
     * @return この予約の generation。0 は返さない。
     * @note UI スレッドから呼ぶこと。
     */
    uint64_t request(std::shared_ptr<const DecodedImage> image);

    /**
     * @brief 完了した認識結果を 1 件取り出す。
     * @return 完了した結果。まだ無ければ std::nullopt。
     * @note UI スレッドから呼ぶこと。取り出した結果は内部から消える。
     */
    std::optional<Completed> takeResult();

    /**
     * @brief 認識完了通知のコールバックを登録する。
     * @param[in] callback 完了時に呼ばれる関数。ワーカースレッド上で呼ばれる。
     * @note 最初の request より前に設定すること。
     */
    void setOnCompleted(std::function<void()> callback);

private:
    /// @brief ワーカースレッドの本体。予約された画像を順に認識する。
    void workerLoop();

    IOcrEngine& engine_;

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    bool stop_ = false;
    uint64_t nextGeneration_ = 1;
    uint64_t pendingGeneration_ = 0;             ///< 0 なら予約なし
    std::shared_ptr<const DecodedImage> pending_;  ///< 次に認識する画像
    std::optional<Completed> completed_;         ///< 未回収の結果(1 件だけ保持)
    std::function<void()> onCompleted_;
    std::thread worker_;
};

} // namespace blinker
