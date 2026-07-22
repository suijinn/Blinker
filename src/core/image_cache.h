#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "platform/decoder.h"

/**
 * @file image_cache.h
 * @brief デコード済み画像の LRU キャッシュと非同期先読み。
 */

namespace blinker {

/// @brief std::filesystem::path を unordered_map のキーにするためのハッシュ関手。
struct PathHash {
    /**
     * @brief パスのハッシュ値を求める。
     * @param[in] p ハッシュ対象のパス。
     * @return std::filesystem::hash_value による値。
     */
    size_t operator()(const std::filesystem::path& p) const {
        return std::filesystem::hash_value(p);
    }
};

/**
 * @brief デコード済み画像の LRU キャッシュ + 非同期先読み。
 *
 * ワーカースレッド 1 本がデコードを担う。setOnDecoded で登録したコールバックは
 * ワーカースレッド上で呼ばれるため、受け側(win 層)は UI スレッドへの通知
 * (PostMessage 等)に変換すること。
 */
class ImageCache {
public:
    /**
     * @brief キャッシュを構築し、ワーカースレッドを起動する。
     * @param[in] decoder  デコードに使う実装。本オブジェクトより長生きすること。
     * @param[in] maxBytes 保持するピクセルデータの合計上限(バイト)。
     * @param[in] maxItems 保持するエントリ数の上限。
     */
    explicit ImageCache(IImageDecoder& decoder, size_t maxBytes = size_t{512} << 20,
                        size_t maxItems = 8);

    /// @brief ワーカースレッドを停止して待ち合わせる。
    ~ImageCache();

    ImageCache(const ImageCache&) = delete;
    ImageCache& operator=(const ImageCache&) = delete;

    /**
     * @brief キャッシュ済みならデコード結果を返す(デコードは行わない)。
     * @param[in]  path   取得する画像のパス。
     * @param[out] failed 非 nullptr のとき、過去にデコード失敗したパスなら true が入る。
     *                    それ以外は false。
     * @param[out] error  非 nullptr のとき、失敗していればデコーダが返した理由が入る。
     *                    失敗していない場合は空文字列になる。
     * @return デコード済み画像。未デコード・失敗時は nullptr。
     */
    std::shared_ptr<DecodedImage> tryGet(const std::filesystem::path& path,
                                         bool* failed = nullptr, std::string* error = nullptr);

    /**
     * @brief 表示対象を最優先でデコード予約する。
     * @param[in] path デコードする画像のパス。
     */
    void requestNow(const std::filesystem::path& path);

    /**
     * @brief 先読み候補を優先度順で差し替える。
     * @param[in] paths 優先度の高い順に並べたパス。所有権を受け取る。
     */
    void setPrefetch(std::vector<std::filesystem::path> paths);

    /**
     * @brief デコード完了通知のコールバックを登録する。
     * @param[in] callback 完了したパスを受け取る関数。ワーカースレッド上で呼ばれる。
     * @note App::openPath より前に設定すること。
     */
    void setOnDecoded(std::function<void(const std::filesystem::path&)> callback);

private:
    /// @brief キャッシュ 1 件分のエントリ。
    struct Entry {
        std::shared_ptr<DecodedImage> image;  ///< デコード結果。失敗時は nullptr
        bool failed = false;                  ///< デコードに失敗したパスか
        std::string error;                    ///< 失敗理由(成功時は空)
        std::list<std::filesystem::path>::iterator lruIt;  ///< lru_ 内の自分の位置
    };

    /// @brief ワーカースレッドの本体。予約されたパスを順にデコードする。
    void workerLoop();

    /**
     * @brief 次にデコードすべきパスを選ぶ(urgent_ を優先)。
     * @return デコード対象のパス。候補がなければ空パス。
     * @pre mutex_ をロック済みであること。
     */
    std::filesystem::path nextTaskLocked() const;

    /**
     * @brief デコード結果をキャッシュへ格納する。
     * @param[in] path  格納するパス。
     * @param[in] image デコード結果。nullptr なら失敗として記録する。
     * @param[in] error 失敗理由(成功時は空文字列)。
     * @pre mutex_ をロック済みであること。
     */
    void storeLocked(const std::filesystem::path& path, std::shared_ptr<DecodedImage> image,
                     std::string error);

    /**
     * @brief 上限を超えた分を LRU 順に破棄する。
     * @pre mutex_ をロック済みであること。
     */
    void evictLocked();

    IImageDecoder& decoder_;
    const size_t maxBytes_;
    const size_t maxItems_;

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    bool stop_ = false;
    std::unordered_map<std::filesystem::path, Entry, PathHash> entries_;
    std::list<std::filesystem::path> lru_;  ///< 先頭が最近使用
    size_t totalBytes_ = 0;
    std::deque<std::filesystem::path> urgent_;
    std::vector<std::filesystem::path> prefetch_;
    std::function<void(const std::filesystem::path&)> onDecoded_;
    std::thread worker_;
};

} // namespace blinker
