#include "core/image_cache.h"

#include <algorithm>

namespace blinker {

namespace fs = std::filesystem;

namespace {

/// 1 フレームだけのフレーム構成を作る(通常の画像・デコード直後の状態)。
std::shared_ptr<const ImageSequence> singleSequence(std::shared_ptr<DecodedImage> image) {
    auto seq = std::make_shared<ImageSequence>();
    seq->frames.push_back(FrameEntry{std::move(image), 0, {}});
    return seq;
}

} // namespace

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

ImageCache::ImageCache(IImageDecoder& decoder, ImageCacheLimits limits, AnimationLimits animation)
    : decoder_(decoder),
      maxBytes_(limits.maxBytes),
      maxItems_(std::max<size_t>(limits.maxItems, 2)),
      animationLimits_(animation) {
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
    if (!it->second.sequence) return nullptr;
    return it->second.sequence->frames.front().image;
}

std::shared_ptr<const ImageSequence> ImageCache::tryGetSequence(const fs::path& path) {
    std::lock_guard lock(mutex_);
    const auto it = entries_.find(path);
    if (it == entries_.end()) return nullptr;
    lru_.splice(lru_.begin(), lru_, it->second.lruIt);
    return it->second.sequence;
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

void ImageCache::requestSequence(const fs::path& path) {
    if (!mayHaveMultipleFrames(path)) return;
    {
        std::lock_guard lock(mutex_);
        const auto it = entries_.find(path);
        // まだデコードできていない・失敗した・調べ済みなら何もしない
        if (it == entries_.end() || !it->second.sequence || it->second.probed) return;
        if (std::find(probe_.begin(), probe_.end(), path) == probe_.end()) {
            probe_.push_back(path);
        }
    }
    wake_.notify_one();
}

void ImageCache::requestFrame(const fs::path& path, const uint32_t index) {
    {
        std::lock_guard lock(mutex_);
        const auto it = entries_.find(path);
        if (it == entries_.end() || !it->second.sequence) return;
        const ImageSequence& seq = *it->second.sequence;
        if (seq.kind != SequenceKind::Pages || index >= seq.frames.size()) return;
        if (seq.frames[index].image) return;  // デコード済み
        const auto pending = std::make_pair(path, index);
        if (std::find(page_.begin(), page_.end(), pending) == page_.end()) {
            page_.push_back(pending);
        }
    }
    wake_.notify_one();
}

void ImageCache::invalidate(const fs::path& path) {
    std::lock_guard lock(mutex_);
    const auto it = entries_.find(path);
    if (it == entries_.end()) return;
    if (it->second.sequence) totalBytes_ -= it->second.sequence->byteSize();
    lru_.erase(it->second.lruIt);
    entries_.erase(it);
    // 捨てた画像に対する後続の仕事はもう意味がない
    std::erase(refine_, path);
    std::erase(probe_, path);
    std::erase(animation_, path);
    std::erase_if(page_, [&path](const auto& e) { return e.first == path; });
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
            switch (task.kind) {
            case TaskKind::Decode: std::erase(urgent_, task.path); break;
            case TaskKind::Probe: std::erase(probe_, task.path); break;
            case TaskKind::Animation: std::erase(animation_, task.path); break;
            case TaskKind::Refine: std::erase(refine_, task.path); break;
            case TaskKind::Page:
                std::erase(page_, std::make_pair(task.path, task.frame));
                break;
            }
        }

        // デコード本体はロックの外で行う(ここが一番時間を食う)
        bool notify = true;
        std::shared_ptr<DecodedImage> image;
        std::shared_ptr<const ImageSequence> animated;
        bool animationTruncated = false;
        SequenceInfo info;
        std::string error;
        switch (task.kind) {
        case TaskKind::Decode:
            image = decoder_.decode(task.path, &error);
            break;
        case TaskKind::Refine:
            image = decoder_.decodeColorManaged(task.path, &error);
            break;
        case TaskKind::Page:
            image = decoder_.decodePage(task.path, task.frame, &error);
            break;
        case TaskKind::Probe:
            info = decoder_.probeSequence(task.path);
            break;
        case TaskKind::Animation: {
            auto seq = std::make_shared<ImageSequence>();
            if (decoder_.decodeAnimation(task.path, animationLimits_, *seq, &error) &&
                seq->frames.size() > 1) {
                animated = std::move(seq);
            } else {
                animationTruncated = seq->truncated;  // 上限超過のときだけ立つ
            }
            break;
        }
        }

        std::function<void(const fs::path&)> callback;
        {
            std::lock_guard lock(mutex_);
            switch (task.kind) {
            case TaskKind::Decode:
                storeLocked(task.path, std::move(image), std::move(error));
                break;
            case TaskKind::Refine:
                notify = storeRefinedLocked(task.path, std::move(image));
                break;
            case TaskKind::Page:
                notify = storePageLocked(task.path, task.frame, std::move(image));
                break;
            case TaskKind::Probe:
                notify = storeProbedLocked(task.path, info);
                break;
            case TaskKind::Animation:
                notify = storeAnimationLocked(task.path, std::move(animated), animationTruncated);
                break;
            }
            callback = onDecoded_;
        }
        if (notify && callback) callback(task.path);
    }
}

ImageCache::Task ImageCache::nextTaskLocked() const {
    for (const auto& p : urgent_) {
        if (!entries_.contains(p)) return Task{p, TaskKind::Decode, 0};
    }
    // 調査は安く、ページ数の案内が出るまでの待ちを短くしたいので先読みより先
    for (const auto& p : probe_) {
        const auto it = entries_.find(p);
        if (it != entries_.end() && it->second.sequence && !it->second.probed) {
            return Task{p, TaskKind::Probe, 0};
        }
    }
    for (const auto& p : prefetch_) {
        if (!entries_.contains(p)) return Task{p, TaskKind::Decode, 0};
    }
    for (const auto& [p, index] : page_) {
        const auto it = entries_.find(p);
        if (it != entries_.end() && it->second.sequence &&
            index < it->second.sequence->frames.size() &&
            !it->second.sequence->frames[index].image) {
            return Task{p, TaskKind::Page, index};
        }
    }
    // 全フレーム展開は時間がかかるので、前後への移動(先読み)を待たせない位置に置く
    for (const auto& p : animation_) {
        const auto it = entries_.find(p);
        if (it != entries_.end() && it->second.sequence &&
            it->second.sequence->kind == SequenceKind::Animation &&
            it->second.sequence->frames.size() <= 1) {
            return Task{p, TaskKind::Animation, 0};
        }
    }
    // 色変換の読み直しは最後。表示も先読みも待っていないときだけ進める
    for (const auto& p : refine_) {
        const auto it = entries_.find(p);
        if (it != entries_.end() && !it->second.refined) return Task{p, TaskKind::Refine, 0};
    }
    return {};
}

void ImageCache::replaceSequenceLocked(Entry& entry,
                                       std::shared_ptr<const ImageSequence> sequence) {
    if (entry.sequence) totalBytes_ -= entry.sequence->byteSize();
    if (sequence) totalBytes_ += sequence->byteSize();
    entry.sequence = std::move(sequence);
}

bool ImageCache::storeRefinedLocked(const fs::path& path, std::shared_ptr<DecodedImage> image) {
    const auto it = entries_.find(path);
    // 読み直している間に捨てられた・別の結果で埋まった場合は何もしない
    if (it == entries_.end() || it->second.refined) return false;
    it->second.refined = true;  // 変換できなかった場合も二度は試さない
    if (!image || !it->second.sequence) return false;
    auto updated = std::make_shared<ImageSequence>(*it->second.sequence);
    updated->frames.front().image = std::move(image);
    replaceSequenceLocked(it->second, std::move(updated));
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
    // 多フレームになりえない形式は調べる必要がない(調査は requestSequence で予約される)
    entry.probed = entry.failed || !mayHaveMultipleFrames(path);
    entry.lruIt = lru_.begin();
    const bool colorPending = image && image->colorPending;
    if (image) replaceSequenceLocked(entry, singleSequence(std::move(image)));
    // 色変換が残っている画像は、手すきになってから読み直して差し替える
    if (colorPending) {
        entry.refined = false;
        refine_.push_back(path);
    }
    entries_.emplace(path, std::move(entry));
    evictLocked();
}

bool ImageCache::storeProbedLocked(const fs::path& path, const SequenceInfo& info) {
    const auto it = entries_.find(path);
    if (it == entries_.end() || !it->second.sequence || it->second.probed) return false;
    it->second.probed = true;  // 結果にかかわらず二度は調べない
    if (info.kind == SequenceKind::Single || info.frameCount < 2) return false;

    auto updated = std::make_shared<ImageSequence>();
    updated->kind = info.kind;
    updated->loopCount = info.loopCount;
    if (info.kind == SequenceKind::Animation) {
        // フレーム数は分かっているが枠だけ先に増やすことはしない。展開が終わるまでは
        // 静止画のまま見せる(「1/24」と出したまま何も動かない状態を作らないため)
        updated->frames.push_back(it->second.sequence->frames.front());
        replaceSequenceLocked(it->second, std::move(updated));
        animation_.push_back(path);
        return false;  // 見た目は何も変わらないので通知しない
    }
    updated->frames.resize(info.frameCount);
    // 先頭ページは既にデコード済み。残りは要求されたときに埋まる
    updated->frames.front().image = it->second.sequence->frames.front().image;
    for (size_t i = 0; i < info.labels.size() && i < updated->frames.size(); ++i) {
        updated->frames[i].label = info.labels[i];
    }
    replaceSequenceLocked(it->second, std::move(updated));
    return true;
}

bool ImageCache::storeAnimationLocked(const fs::path& path,
                                      std::shared_ptr<const ImageSequence> sequence,
                                      const bool truncated) {
    const auto it = entries_.find(path);
    if (it == entries_.end() || !it->second.sequence) return false;
    if (it->second.sequence->frames.size() > 1) return false;  // 既に展開済み
    if (!sequence) {
        // 上限超過・展開失敗。中途半端に途中まで再生すると不具合にしか見えないので、
        // 先頭フレームだけの静止画へ落とす(truncated なら App が理由を案内する)
        auto still = std::make_shared<ImageSequence>();
        still->kind = SequenceKind::Single;
        still->truncated = truncated;
        still->frames.push_back(it->second.sequence->frames.front());
        replaceSequenceLocked(it->second, std::move(still));
        return truncated;  // 見た目が変わらない場合は通知しない
    }
    replaceSequenceLocked(it->second, std::move(sequence));
    evictLocked();
    return true;
}

bool ImageCache::storePageLocked(const fs::path& path, const uint32_t index,
                                 std::shared_ptr<DecodedImage> image) {
    const auto it = entries_.find(path);
    if (it == entries_.end() || !it->second.sequence || !image) return false;
    if (index >= it->second.sequence->frames.size()) return false;
    if (it->second.sequence->frames[index].image) return false;  // 既に埋まっている
    auto updated = std::make_shared<ImageSequence>(*it->second.sequence);
    updated->frames[index].image = std::move(image);
    replaceSequenceLocked(it->second, std::move(updated));
    evictLocked();
    return true;
}

void ImageCache::evictLocked() {
    while (entries_.size() > maxItems_ || totalBytes_ > maxBytes_) {
        if (lru_.size() <= 1) break;  // 直近の1枚(表示中の可能性が高い)は残す
        const fs::path victim = lru_.back();
        const auto it = entries_.find(victim);
        if (it != entries_.end()) {
            if (it->second.sequence) totalBytes_ -= it->second.sequence->byteSize();
            entries_.erase(it);
        }
        std::erase(refine_, victim);
        std::erase(probe_, victim);
        std::erase(animation_, victim);
        std::erase_if(page_, [&victim](const auto& e) { return e.first == victim; });
        lru_.pop_back();
    }
}

} // namespace blinker
