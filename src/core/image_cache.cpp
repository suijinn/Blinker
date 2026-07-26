#include "core/image_cache.h"

#include <algorithm>

namespace blinker {

namespace fs = std::filesystem;

ImageCacheLimits cacheLimitsFromConfig(const Config& config) {
    const ImageCacheLimits defaults;
    const int memoryMB = std::clamp(
        config.getInt("cache", "max_memory_mb",
                      static_cast<int>(defaults.maxBytes >> 20)),
        kMinCacheMemoryMB, kMaxCacheMemoryMB);
    const int items = std::clamp(
        config.getInt("cache", "max_items", static_cast<int>(defaults.maxItems)),
        kMinCacheItems, kMaxCacheItems);
    return ImageCacheLimits{static_cast<size_t>(memoryMB) << 20, static_cast<size_t>(items)};
}

ImageCache::ImageCache(IImageDecoder& decoder, ImageCacheLimits limits)
    : decoder_(decoder),
      maxBytes_(limits.maxBytes),
      maxItems_(std::max<size_t>(limits.maxItems, 2)) {
    worker_ = std::thread(&ImageCache::workerLoop, this);
}

ImageCache::~ImageCache() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    worker_.join();
}

std::shared_ptr<DecodedImage> ImageCache::tryGet(const fs::path& path, bool* failed,
                                                 std::string* error) {
    if (failed) *failed = false;
    if (error) error->clear();
    std::lock_guard lock(mutex_);
    const auto it = entries_.find(path);
    if (it == entries_.end()) return nullptr;
    lru_.splice(lru_.begin(), lru_, it->second.lruIt);  // 最近使用に更新
    if (failed) *failed = it->second.failed;
    if (error) *error = it->second.error;
    return it->second.image;
}

void ImageCache::requestNow(const fs::path& path) {
    {
        std::lock_guard lock(mutex_);
        if (entries_.contains(path)) return;
        if (std::find(urgent_.begin(), urgent_.end(), path) == urgent_.end()) {
            urgent_.push_front(path);
        }
    }
    wake_.notify_one();
}

void ImageCache::invalidate(const fs::path& path) {
    std::lock_guard lock(mutex_);
    const auto it = entries_.find(path);
    if (it == entries_.end()) return;
    if (it->second.image) totalBytes_ -= it->second.image->byteSize();
    lru_.erase(it->second.lruIt);
    entries_.erase(it);
    std::erase(refine_, path);  // 捨てた画像を読み直す意味はない
}

void ImageCache::setPrefetch(std::vector<fs::path> paths) {
    {
        std::lock_guard lock(mutex_);
        prefetch_ = std::move(paths);
    }
    wake_.notify_one();
}

void ImageCache::setOnDecoded(std::function<void(const fs::path&)> callback) {
    std::lock_guard lock(mutex_);
    onDecoded_ = std::move(callback);
}

void ImageCache::workerLoop() {
    for (;;) {
        Task task;
        {
            std::unique_lock lock(mutex_);
            for (;;) {
                if (stop_) return;
                task = nextTaskLocked();
                if (!task.path.empty()) break;
                wake_.wait(lock);
            }
            // 取り出したタスクをキューから除去(先読みは prefetch_ に残っていてよい)
            std::erase(urgent_, task.path);
            std::erase(refine_, task.path);
        }

        bool notify = true;
        std::shared_ptr<DecodedImage> image;
        std::string error;
        if (task.refine) {
            image = decoder_.decodeColorManaged(task.path, &error);
        } else {
            image = decoder_.decode(task.path, &error);
        }

        std::function<void(const fs::path&)> callback;
        {
            std::lock_guard lock(mutex_);
            if (task.refine) {
                notify = storeRefinedLocked(task.path, std::move(image));
            } else {
                storeLocked(task.path, std::move(image), std::move(error));
            }
            callback = onDecoded_;
        }
        if (notify && callback) callback(task.path);
    }
}

ImageCache::Task ImageCache::nextTaskLocked() const {
    for (const auto& p : urgent_) {
        if (!entries_.contains(p)) return Task{p, false};
    }
    for (const auto& p : prefetch_) {
        if (!entries_.contains(p)) return Task{p, false};
    }
    // 色変換の読み直しは最後。表示も先読みも待っていないときだけ進める
    for (const auto& p : refine_) {
        const auto it = entries_.find(p);
        if (it != entries_.end() && !it->second.refined) return Task{p, true};
    }
    return {};
}

bool ImageCache::storeRefinedLocked(const fs::path& path, std::shared_ptr<DecodedImage> image) {
    const auto it = entries_.find(path);
    // 読み直している間に捨てられた・別の結果で埋まった場合は何もしない
    if (it == entries_.end() || it->second.refined) return false;
    it->second.refined = true;  // 変換できなかった場合も二度は試さない
    if (!image) return false;
    if (it->second.image) totalBytes_ -= it->second.image->byteSize();
    totalBytes_ += image->byteSize();
    it->second.image = std::move(image);
    evictLocked();
    return true;
}

void ImageCache::storeLocked(const fs::path& path, std::shared_ptr<DecodedImage> image,
                             std::string error) {
    if (entries_.contains(path)) return;
    lru_.push_front(path);
    Entry entry;
    entry.failed = (image == nullptr);
    if (entry.failed) entry.error = std::move(error);
    entry.image = std::move(image);
    entry.lruIt = lru_.begin();
    if (entry.image) totalBytes_ += entry.image->byteSize();
    // 色変換が残っている画像は、手すきになってから読み直して差し替える
    if (entry.image && entry.image->colorPending) {
        entry.refined = false;
        refine_.push_back(path);
    }
    entries_.emplace(path, std::move(entry));
    evictLocked();
}

void ImageCache::evictLocked() {
    while (entries_.size() > maxItems_ || totalBytes_ > maxBytes_) {
        if (lru_.size() <= 1) break;  // 直近の1枚(表示中の可能性が高い)は残す
        const fs::path victim = lru_.back();
        const auto it = entries_.find(victim);
        if (it != entries_.end()) {
            if (it->second.image) totalBytes_ -= it->second.image->byteSize();
            entries_.erase(it);
        }
        std::erase(refine_, victim);
        lru_.pop_back();
    }
}

} // namespace blinker
