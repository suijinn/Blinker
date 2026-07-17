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

namespace blinker {

struct PathHash {
    size_t operator()(const std::filesystem::path& p) const {
        return std::filesystem::hash_value(p);
    }
};

// デコード済み画像の LRU キャッシュ+非同期先読み。ワーカースレッド1本がデコードを担う。
// onDecoded はワーカースレッド上で呼ばれるため、受け側(win層)は UI スレッドへの
// 通知(PostMessage 等)に変換すること。
class ImageCache {
public:
    explicit ImageCache(IImageDecoder& decoder, size_t maxBytes = size_t{512} << 20,
                        size_t maxItems = 8);
    ~ImageCache();

    ImageCache(const ImageCache&) = delete;
    ImageCache& operator=(const ImageCache&) = delete;

    // キャッシュ済みならデコード結果を返す。過去に失敗したパスは *failed = true になる。
    std::shared_ptr<DecodedImage> tryGet(const std::filesystem::path& path,
                                         bool* failed = nullptr);
    // 表示対象を最優先でデコード予約する
    void requestNow(const std::filesystem::path& path);
    // 先読み候補を優先度順で差し替える
    void setPrefetch(std::vector<std::filesystem::path> paths);
    // デコード完了通知(ワーカースレッド上で呼ばれる)。openPath より前に設定すること
    void setOnDecoded(std::function<void(const std::filesystem::path&)> callback);

private:
    struct Entry {
        std::shared_ptr<DecodedImage> image;  // デコード失敗時は nullptr
        bool failed = false;
        std::list<std::filesystem::path>::iterator lruIt;
    };

    void workerLoop();
    std::filesystem::path nextTaskLocked() const;
    void storeLocked(const std::filesystem::path& path, std::shared_ptr<DecodedImage> image);
    void evictLocked();

    IImageDecoder& decoder_;
    const size_t maxBytes_;
    const size_t maxItems_;

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    bool stop_ = false;
    std::unordered_map<std::filesystem::path, Entry, PathHash> entries_;
    std::list<std::filesystem::path> lru_;  // 先頭が最近使用
    size_t totalBytes_ = 0;
    std::deque<std::filesystem::path> urgent_;
    std::vector<std::filesystem::path> prefetch_;
    std::function<void(const std::filesystem::path&)> onDecoded_;
    std::thread worker_;
};

} // namespace blinker
