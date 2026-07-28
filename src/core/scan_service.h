#pragma once

#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

#include "platform/file_system.h"

/**
 * @file scan_service.h
 * @brief フォルダ列挙の非同期実行(サブフォルダの再帰走査用)。
 */

namespace blinker {

/**
 * @brief フォルダ列挙を UI スレッドの外で走らせる。
 *
 * ワーカースレッド 1 本が列挙を担う(ImageCache / OcrService と同じ形)。
 * setOnCompleted で登録したコールバックはワーカースレッド上で呼ばれるため、
 * 受け側(win/sdl 層)は UI スレッドへの通知 (PostMessage 等) に変換すること。
 *
 * サブフォルダの再帰走査は数千〜数万件になると数百ミリ秒以上かかり、UI スレッドで
 * 直接呼ぶと起動が固まる(設計目標の「高速起動」に反する)。App はまず直下だけを
 * 同期列挙して表示を確定し、再帰が有効なときだけこの経路で全体を読み直す。
 */
class ScanService {
public:
    /// @brief 1 件の列挙結果。
    struct Completed {
        uint64_t generation = 0;       ///< 対応する request の戻り値
        std::filesystem::path root;    ///< 列挙の起点フォルダ
        ListOptions options;           ///< 実行したオプション
        ListResult result;             ///< 列挙結果
    };

    /**
     * @brief サービスを構築し、ワーカースレッドを起動する。
     * @param[in] fileSystem 列挙に使う実装。本オブジェクトより長生きすること。
     */
    explicit ScanService(IFileSystem& fileSystem);

    /// @brief ワーカースレッドを停止して待ち合わせる(実行中の列挙の完了を待つ)。
    ~ScanService();

    ScanService(const ScanService&) = delete;
    ScanService& operator=(const ScanService&) = delete;

    /**
     * @brief 列挙を予約する。
     *
     * 未処理の予約が残っていれば置き換える(最後の 1 件だけが実行される)。
     * 既に走り出した列挙は止められないため、その結果は古い generation を持って
     * 完了する。呼び出し側は takeResult の generation で捨てること。
     *
     * @param[in] root    列挙の起点フォルダ。
     * @param[in] options 再帰の有無と件数上限。
     * @return この予約の generation。0 は返さない。
     * @note UI スレッドから呼ぶこと。
     */
    uint64_t request(std::filesystem::path root, ListOptions options);

    /**
     * @brief 完了した列挙結果を 1 件取り出す。
     * @return 完了した結果。まだ無ければ std::nullopt。
     * @note UI スレッドから呼ぶこと。取り出した結果は内部から消える。
     */
    std::optional<Completed> takeResult();

    /**
     * @brief 列挙完了通知のコールバックを登録する。
     * @param[in] callback 完了時に呼ばれる関数。ワーカースレッド上で呼ばれる。
     * @note 最初の request より前に設定すること。
     */
    void setOnCompleted(std::function<void()> callback);

private:
    /// @brief ワーカースレッドの本体。予約されたフォルダを順に列挙する。
    void workerLoop();

    IFileSystem& fileSystem_;

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    bool stop_ = false;
    uint64_t nextGeneration_ = 1;
    uint64_t pendingGeneration_ = 0;      ///< 0 なら予約なし
    std::filesystem::path pendingRoot_;   ///< 次に列挙するフォルダ
    ListOptions pendingOptions_;          ///< 次の列挙のオプション
    std::optional<Completed> completed_;  ///< 未回収の結果(1 件だけ保持)
    std::function<void()> onCompleted_;
    std::thread worker_;
};

} // namespace blinker
