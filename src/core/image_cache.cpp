#include "core/image_cache.h"

#include <algorithm>

namespace blinker {

namespace fs = std::filesystem;

ImageCache::ImageCache(IImageDecoder& decoder, size_t maxBytes, size_t maxItems)
    : decoder_(decoder), maxBytes_(maxBytes), maxItems_(std::max<size_t>(maxItems, 2)) {
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

std::shared_ptr<DecodedImage> ImageCache::tryGet(const fs::path& path, bool* failed) {
    if (failed) *failed = false;
    std::lock_guard lock(mutex_);
    const auto it = entries_.find(path);
    if (it == entries_.end()) return nullptr;
    lru_.splice(lru_.begin(), lru_, it->second.lruIt);  // 最近使用に更新
    if (failed) *failed = it->second.failed;
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
        fs::path task;
        {
            std::unique_lock lock(mutex_);
            for (;;) {
                if (stop_) return;
                task = nextTaskLocked();
                if (!task.empty()) break;
                wake_.wait(lock);
            }
            // 取り出した urgent タスクをキューから除去(先読みは prefetch_ に残っていてよい)
            std::erase(urgent_, task);
        }

        std::shared_ptr<DecodedImage> image = decoder_.decode(task);

        std::function<void(const fs::path&)> callback;
        {
            std::lock_guard lock(mutex_);
            storeLocked(task, std::move(image));
            callback = onDecoded_;
        }
        if (callback) callback(task);
    }
}

std::filesystem::path ImageCache::nextTaskLocked() const {
    for (const auto& p : urgent_) {
        if (!entries_.contains(p)) return p;
    }
    for (const auto& p : prefetch_) {
        if (!entries_.contains(p)) return p;
    }
    return {};
}

void ImageCache::storeLocked(const fs::path& path, std::shared_ptr<DecodedImage> image) {
    if (entries_.contains(path)) return;
    lru_.push_front(path);
    Entry entry;
    entry.failed = (image == nullptr);
    entry.image = std::move(image);
    entry.lruIt = lru_.begin();
    if (entry.image) totalBytes_ += entry.image->byteSize();
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
        lru_.pop_back();
    }
}

} // namespace blinker
