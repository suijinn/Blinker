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

#include "core/config.h"
#include "platform/decoder.h"

/**
 * @file image_cache.h
 * @brief デコード済み画像の LRU キャッシュと非同期先読み。
 */

namespace blinker {

/**
 * @brief ImageCache が保持する量の上限。
 *
 * 既定値の正はここ。blinker.ini の `[cache]` を読む cacheLimitsFromConfig も
 * この既定値を土台にする。
 */
struct ImageCacheLimits {
    size_t maxBytes = size_t{512} << 20;  ///< ピクセルデータの合計上限(バイト)
    size_t maxItems = 8;                  ///< 保持するエントリ数の上限
};

/// @brief cacheLimitsFromConfig が受け付ける値の範囲(範囲外は丸める)。
constexpr int kMinCacheMemoryMB = 32;     ///< 上限メモリの下限 (MB)
constexpr int kMaxCacheMemoryMB = 16384;  ///< 上限メモリの上限 (MB)
constexpr int kMinCacheItems = 2;         ///< 保持枚数の下限(表示中と隣の 1 枚)
constexpr int kMaxCacheItems = 64;        ///< 保持枚数の上限

/**
 * @brief blinker.ini の `[cache]` から容量上限を読む。
 *
 * `max_memory_mb`(MB 単位)と `max_items`(枚数)を見る。どちらも範囲外の値は
 * 丸め、キーが無ければ ImageCacheLimits の既定値をそのまま使う。
 *
 * @param[in] config 読み込み済みの設定。
 * @return 適用する上限。
 */
ImageCacheLimits cacheLimitsFromConfig(const Config& config);

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
     * @param[in] decoder デコードに使う実装。本オブジェクトより長生きすること。
     * @param[in] limits  保持量の上限。省略時は ImageCacheLimits の既定値。
     * @note maxItems は 2 未満を渡しても 2 として扱う(表示中の 1 枚しか持てないと
     *       前後へ移るたびに再デコードになる)。
     */
    explicit ImageCache(IImageDecoder& decoder, ImageCacheLimits limits = {});

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
     * @brief 1 件のキャッシュを捨てる(ファイルが書き換わったとき)。
     *
     * 上書き保存の後に呼ぶ。捨てておかないと、別の画像へ移って戻ったときに
     * 保存前のピクセルが再表示されてしまう。次に必要になった時点で再デコードされる。
     *
     * @param[in] path 捨てるパス。キャッシュに無ければ何もしない。
     * @note 同じパスのデコードがワーカーで進行中だった場合は、その結果が
     *       完了時に入り直す(表示中の画像は既にデコード済みなので実際には起こらない)。
     */
    void invalidate(const std::filesystem::path& path);

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
        bool refined = true;  ///< 色変換のための読み直しが済んだ(または不要)か
        std::list<std::filesystem::path>::iterator lruIt;  ///< lru_ 内の自分の位置
    };

    /// @brief ワーカースレッドが処理する 1 件の仕事。
    struct Task {
        std::filesystem::path path;  ///< 対象のパス。空なら仕事なし
        bool refine = false;         ///< true なら色変換のための読み直し
    };

    /// @brief ワーカースレッドの本体。予約されたパスを順にデコードする。
    void workerLoop();

    /**
     * @brief 次の仕事を選ぶ(urgent_ → prefetch_ → refine_ の順)。
     *
     * 色変換の読み直しは最も優先度が低い。表示や先読みを待たせないため。
     *
     * @return 処理する仕事。候補がなければ path が空。
     * @pre mutex_ をロック済みであること。
     */
    Task nextTaskLocked() const;

    /**
     * @brief 色変換で読み直した結果をエントリへ反映する。
     * @param[in] path  対象のパス。既に捨てられていれば何もしない。
     * @param[in] image 変換済み画像。nullptr なら「変換できなかった」として印だけ付ける。
     * @return 差し替えたら true(呼び出し側は完了通知を出す)。
     * @pre mutex_ をロック済みであること。
     */
    bool storeRefinedLocked(const std::filesystem::path& path,
                            std::shared_ptr<DecodedImage> image);

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
    /// 色変換のために読み直す待ち行列(`DecodedImage::colorPending` が立った分)
    std::deque<std::filesystem::path> refine_;
    std::function<void(const std::filesystem::path&)> onDecoded_;
    std::thread worker_;
};

} // namespace blinker
