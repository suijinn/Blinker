#include "core/scan_service.h"

#include <utility>

namespace blinker {

ScanService::ScanService(IFileSystem& fileSystem) : fileSystem_(fileSystem) {
    worker_ = std::thread(&ScanService::workerLoop, this);
}

ScanService::~ScanService() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    worker_.join();
}

uint64_t ScanService::request(std::filesystem::path root, const ListOptions options) {
    uint64_t generation = 0;
    {
        std::lock_guard lock(mutex_);
        generation = nextGeneration_++;
        pendingGeneration_ = generation;
        pendingRoot_ = std::move(root);
        pendingOptions_ = options;
    }
    wake_.notify_one();
    return generation;
}

std::optional<ScanService::Completed> ScanService::takeResult() {
    std::lock_guard lock(mutex_);
    return std::exchange(completed_, std::nullopt);
}

void ScanService::setOnCompleted(std::function<void()> callback) {
    std::lock_guard lock(mutex_);
    onCompleted_ = std::move(callback);
}

void ScanService::workerLoop() {
    for (;;) {
        std::filesystem::path root;
        ListOptions options;
        uint64_t generation = 0;
        {
            std::unique_lock lock(mutex_);
            while (!stop_ && pendingGeneration_ == 0) wake_.wait(lock);
            if (stop_) return;
            root = std::move(pendingRoot_);
            options = pendingOptions_;
            generation = pendingGeneration_;
            pendingRoot_.clear();
            pendingGeneration_ = 0;
        }

        Completed done;
        done.generation = generation;
        done.options = options;
        done.result = fileSystem_.listImages(root, options);
        done.root = std::move(root);

        std::function<void()> callback;
        {
            std::lock_guard lock(mutex_);
            // 未回収の結果があっても新しいもので上書きする。UI が拾うのは常に最新の 1 件
            completed_ = std::move(done);
            callback = onCompleted_;
        }
        if (callback) callback();
    }
}

} // namespace blinker
