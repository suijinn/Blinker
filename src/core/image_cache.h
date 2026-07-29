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

#include <utility>

#include "core/config.h"
#include "platform/decoder.h"
#include "platform/image_formats.h"

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
 * エントリの単位は**ファイル 1 つ**で、値は `ImageSequence`(1 枚以上のフレーム)。
 * 通常の画像は 1 フレームだけの列になるので、既存の tryGet / requestNow の使い方は
 * 変わらない。多ページ TIFF・ICO・アニメーション GIF だけがフレームを増やす。
 *
 * ワーカースレッド 1 本がデコードを担う。setOnDecoded で登録したコールバックは
 * ワーカースレッド上で呼ばれるため、受け側(win 層)は UI スレッドへの通知
 * (PostMessage 等)に変換すること。
 *
 * @note 格納した `ImageSequence` は**書き換えない**。フレームが増えるたびに作り直して
 *       差し替える(コピーオンライト)ので、UI スレッドが持ち出した shared_ptr は
 *       ロックなしで読み続けられる。
 */
class ImageCache {
public:
    /**
     * @brief キャッシュを構築し、ワーカースレッドを起動する。
     * @param[in] decoder   デコードに使う実装。本オブジェクトより長生きすること。
     * @param[in] limits    保持量の上限。省略時は ImageCacheLimits の既定値。
     * @param[in] animation アニメーションを展開するときの上限。省略時は既定値。
     * @note maxItems は 2 未満を渡しても 2 として扱う(表示中の 1 枚しか持てないと
     *       前後へ移るたびに再デコードになる)。
     */
    explicit ImageCache(IImageDecoder& decoder, ImageCacheLimits limits = {},
                        AnimationLimits animation = {});

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
     * @brief キャッシュ済みならフレーム構成を返す(デコードは行わない)。
     * @param[in] path 取得する画像のパス。
     * @return フレーム構成。未デコード・失敗時は nullptr。
     * @note 返した後にフレームが増えても、この shared_ptr が指す中身は変わらない。
     *       増えると完了通知が来るので、そのたびに取り直すこと。
     */
    std::shared_ptr<const ImageSequence> tryGetSequence(const std::filesystem::path& path);

    /**
     * @brief 表示対象を最優先でデコード予約する。
     * @param[in] path デコードする画像のパス。
     */
    void requestNow(const std::filesystem::path& path);

    /**
     * @brief 表示中になったパスのフレーム構成を調べるよう予約する。
     *
     * 多フレームになりうる拡張子(`mayHaveMultipleFrames`)でなければ何もしない。
     * 先読みでデコードした画像は構成を調べていないので、表示に採用した時点で呼ぶこと。
     * アニメーションと分かった場合は続けて全フレームの展開も予約される。
     *
     * @param[in] path 対象のパス。未デコードなら何もしない(デコード後に呼び直すこと)。
     */
    void requestSequence(const std::filesystem::path& path);

    /**
     * @brief 独立ページ(多ページ TIFF・ICO)のデコードを予約する。
     * @param[in] path  対象のパス。
     * @param[in] index ページ番号(0 起点)。範囲外・デコード済みなら何もしない。
     */
    void requestFrame(const std::filesystem::path& path, uint32_t index);

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
    /// @brief キャッシュ 1 件分のエントリ(= ファイル 1 つ)。
    struct Entry {
        std::shared_ptr<const ImageSequence> sequence;  ///< フレーム構成。失敗時は nullptr
        bool failed = false;                  ///< デコードに失敗したパスか
        std::string error;                    ///< 失敗理由(成功時は空)
        bool refined = true;  ///< 色変換のための読み直しが済んだ(または不要)か
        bool probed = false;  ///< フレーム構成を調べ終えた(または調べる必要がない)か
        std::list<std::filesystem::path>::iterator lruIt;  ///< lru_ 内の自分の位置
    };

    /// @brief ワーカースレッドが処理する仕事の種類。
    enum class TaskKind {
        Decode,     ///< 先頭フレームのデコード(表示・先読み)
        Probe,      ///< フレーム構成の調査
        Page,       ///< 独立ページのデコード
        Animation,  ///< アニメーションの全フレーム展開
        Refine,     ///< 色変換のための読み直し
    };

    /// @brief ワーカースレッドが処理する 1 件の仕事。
    struct Task {
        std::filesystem::path path;         ///< 対象のパス。空なら仕事なし
        TaskKind kind = TaskKind::Decode;   ///< 仕事の種類
        uint32_t frame = 0;                 ///< Page のときのページ番号
    };

    /// @brief ワーカースレッドの本体。予約された仕事を順に処理する。
    void workerLoop();

    /**
     * @brief 次の仕事を選ぶ。
     *
     * 優先順位は urgent_ → probe_ → prefetch_ → page_ → animation_ → refine_。
     * 調査は安くて表示(ページ数の案内)を待たせるので先読みより上、全フレーム展開は
     * 前後への移動の軽さを優先して先読みより下、色変換の読み直しは従来どおり最後。
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
     * @brief 調査結果をエントリへ反映し、フレームの枠を用意する。
     * @param[in] path 対象のパス。既に捨てられていれば何もしない。
     * @param[in] info 調査結果。
     * @return フレーム構成が変わったら true(呼び出し側は完了通知を出す)。
     * @pre mutex_ をロック済みであること。
     */
    bool storeProbedLocked(const std::filesystem::path& path, const SequenceInfo& info);

    /**
     * @brief 展開したアニメーションをエントリへ反映する。
     * @param[in] path      対象のパス。既に捨てられていれば何もしない。
     * @param[in] sequence  展開結果。上限超過・失敗なら nullptr(静止画へ落とす)。
     * @param[in] truncated 上限超過で諦めた場合に true(App が理由を案内する)。
     * @return 表示に反映すべき変化があれば true(呼び出し側は完了通知を出す)。
     * @pre mutex_ をロック済みであること。
     */
    bool storeAnimationLocked(const std::filesystem::path& path,
                              std::shared_ptr<const ImageSequence> sequence, bool truncated);

    /**
     * @brief デコードしたページをエントリへ反映する。
     * @param[in] path  対象のパス。既に捨てられていれば何もしない。
     * @param[in] index ページ番号。
     * @param[in] image デコード結果。nullptr なら何もしない。
     * @return 埋められたら true(呼び出し側は完了通知を出す)。
     * @pre mutex_ をロック済みであること。
     */
    bool storePageLocked(const std::filesystem::path& path, uint32_t index,
                         std::shared_ptr<DecodedImage> image);

    /**
     * @brief エントリのフレーム構成を差し替え、バイト数の集計を合わせる。
     * @param[in,out] entry    差し替えるエントリ。
     * @param[in]     sequence 新しいフレーム構成。
     * @pre mutex_ をロック済みであること。
     */
    void replaceSequenceLocked(Entry& entry, std::shared_ptr<const ImageSequence> sequence);

    /**
     * @brief 上限を超えた分を LRU 順に破棄する。
     * @pre mutex_ をロック済みであること。
     */
    void evictLocked();

    IImageDecoder& decoder_;
    const size_t maxBytes_;
    const size_t maxItems_;
    const AnimationLimits animationLimits_;

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    bool stop_ = false;
    std::unordered_map<std::filesystem::path, Entry, PathHash> entries_;
    std::list<std::filesystem::path> lru_;  ///< 先頭が最近使用
    size_t totalBytes_ = 0;
    std::deque<std::filesystem::path> urgent_;
    std::deque<std::filesystem::path> probe_;      ///< フレーム構成を調べる待ち行列
    std::vector<std::filesystem::path> prefetch_;
    /// 独立ページのデコード待ち行列(パスとページ番号)
    std::deque<std::pair<std::filesystem::path, uint32_t>> page_;
    std::deque<std::filesystem::path> animation_;  ///< 全フレーム展開の待ち行列
    /// 色変換のために読み直す待ち行列(`DecodedImage::colorPending` が立った分)
    std::deque<std::filesystem::path> refine_;
    std::function<void(const std::filesystem::path&)> onDecoded_;
    std::thread worker_;
};

} // namespace blinker
