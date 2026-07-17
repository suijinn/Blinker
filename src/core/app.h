#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "core/command.h"
#include "core/config.h"
#include "core/geometry.h"
#include "core/image_cache.h"
#include "core/image_list.h"
#include "core/keymap.h"
#include "core/viewport.h"
#include "platform/file_system.h"

namespace blinker {

// App がウィンドウ層に要求するサービス。win 層 (MainWindow) が実装する。
class IAppHost {
public:
    virtual ~IAppHost() = default;
    virtual void requestRedraw() = 0;
    virtual void setTitle(const std::wstring& title) = 0;
    virtual void setFullscreen(bool enabled) = 0;
    virtual bool isFullscreen() const = 0;
    virtual std::optional<std::filesystem::path> showOpenDialog() = 0;
    virtual void quit() = 0;
};

// アプリ本体の状態機械。入力は Command に正規化されて execute() に集まり、
// 状態を更新して host にタイトル変更・再描画を依頼する(一方向フロー)。
class App {
public:
    App(IAppHost& host, IFileSystem& fileSystem, ImageCache& cache);

    void applyConfig(const Config& config);

    void openPath(const std::filesystem::path& path);  // 画像ファイル or フォルダ
    void execute(Command command);
    bool onKey(const KeyChord& chord);  // バインドがあれば実行して true
    void onResize(float width, float height);
    void onWheel(float wheelNotches, Point screenPos);  // 正で拡大
    void onDragPan(float dx, float dy);
    void onDecodeCompleted();  // UI スレッドで呼ぶこと

    // 描画用スナップショット
    const std::shared_ptr<DecodedImage>& currentImage() const { return current_; }
    Matrix3x2 imageToScreen() const { return viewport_.imageToScreen(); }
    float zoom() const { return viewport_.zoom(); }
    uint32_t backgroundRGB() const { return backgroundRGB_; }

private:
    static constexpr float kPanStepPx = 64.0f;

    void refreshCurrent();
    void onViewChanged();
    void updatePrefetch();
    void updateTitle();

    IAppHost& host_;
    IFileSystem& fileSystem_;
    ImageCache& cache_;
    Keymap keymap_ = Keymap::defaults();
    ImageList list_;
    Viewport viewport_;
    std::shared_ptr<DecodedImage> current_;
    std::filesystem::path displayedPath_;  // current_ がどのパスの画像か
    bool loadFailed_ = false;
    uint32_t backgroundRGB_ = 0x202020;
    int prefetchRadius_ = 2;
};

} // namespace blinker
