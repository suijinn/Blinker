#include "core/ocr_service.h"

#include <utility>

namespace blinker {

OcrService::OcrService(IOcrEngine& engine) : engine_(engine) {
    worker_ = std::thread(&OcrService::workerLoop, this);
}

OcrService::~OcrService() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    worker_.join();
}

uint64_t OcrService::request(std::shared_ptr<const DecodedImage> image) {
    uint64_t generation = 0;
    {
        std::lock_guard lock(mutex_);
        generation = nextGeneration_++;
        pendingGeneration_ = generation;
        pending_ = std::move(image);
    }
    wake_.notify_one();
    return generation;
}

std::optional<OcrService::Completed> OcrService::takeResult() {
    std::lock_guard lock(mutex_);
    return std::exchange(completed_, std::nullopt);
}

void OcrService::setOnCompleted(std::function<void()> callback) {
    std::lock_guard lock(mutex_);
    onCompleted_ = std::move(callback);
}

void OcrService::workerLoop() {
    for (;;) {
        std::shared_ptr<const DecodedImage> image;
        uint64_t generation = 0;
        {
            std::unique_lock lock(mutex_);
            while (!stop_ && pendingGeneration_ == 0) wake_.wait(lock);
            if (stop_) return;
            image = std::move(pending_);
            generation = pendingGeneration_;
            pending_.reset();
            pendingGeneration_ = 0;
        }

        Completed done;
        done.generation = generation;
        if (image) {
            done.ok = engine_.recognize(*image, &done.result, &done.error);
        } else {
            done.error = "認識する画像がありません";
        }

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
