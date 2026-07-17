// core 層の単体テスト。フレームワーク不使用の軽量 CHECK マクロで検証する。
// 実行: build/<preset>/tests/core_tests.exe (ctest からも起動される)

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <iostream>
#include <mutex>

#include "core/app.h"
#include "core/config.h"
#include "core/geometry.h"
#include "core/image_cache.h"
#include "core/image_list.h"
#include "core/keymap.h"
#include "core/viewport.h"

namespace {

int g_failures = 0;

void checkImpl(bool ok, const char* expr, const char* file, int line) {
    if (!ok) {
        ++g_failures;
        std::cout << "FAIL " << file << ":" << line << "  " << expr << "\n";
    }
}

#define CHECK(cond) checkImpl((cond), #cond, __FILE__, __LINE__)

bool nearly(float a, float b, float eps = 0.001f) {
    return std::fabs(a - b) <= eps;
}

using namespace blinker;

void testMatrix() {
    // 平行移動→スケールの合成 (行ベクトル規約: 左から順に適用)
    const Matrix3x2 m = Matrix3x2::translation(10, 20) * Matrix3x2::scale(2);
    const Point p = m.apply({1, 1});
    CHECK(nearly(p.x, 22));
    CHECK(nearly(p.y, 42));

    // 時計回り90度: (1,0) → (0,1)  (Y下向きスクリーン座標系)
    const Point r = Matrix3x2::rotation90(1).apply({1, 0});
    CHECK(nearly(r.x, 0));
    CHECK(nearly(r.y, 1));

    // 4回転で元に戻る
    const Point r4 = Matrix3x2::rotation90(4).apply({3, 7});
    CHECK(nearly(r4.x, 3));
    CHECK(nearly(r4.y, 7));
}

void testViewportFit() {
    Viewport vp;
    vp.setWindowSize({800, 600});
    vp.setImage({1600, 600});
    CHECK(vp.fitMode());
    CHECK(nearly(vp.zoom(), 0.5f));  // 幅方向に律速

    // 画像中心はウィンドウ中心へ
    const Point center = vp.imageToScreen().apply({800, 300});
    CHECK(nearly(center.x, 400));
    CHECK(nearly(center.y, 300));

    // 小さい画像は既定では等倍のまま(拡大しない)
    vp.setImage({100, 100});
    CHECK(nearly(vp.zoom(), 1.0f));

    // fit_upscale 有効なら拡大する
    vp.setFitUpscale(true);
    CHECK(nearly(vp.zoom(), 6.0f));
    vp.setFitUpscale(false);

    // 回転すると縦横が入れ替わってフィット率が変わる
    vp.setImage({1600, 600});
    vp.rotate(1);
    CHECK(vp.rotationDegrees() == 90);
    CHECK(nearly(vp.zoom(), 600.0f / 1600.0f));  // 高さ方向に律速
}

void testViewportZoomAt() {
    Viewport vp;
    vp.setWindowSize({800, 600});
    vp.setImage({1600, 1200});
    CHECK(nearly(vp.zoom(), 0.5f));

    // カーソル位置 (200,150) 直下の画像点はズーム後も同じスクリーン位置に留まる
    const Point imagePoint{400, 300};  // ズーム前に (200,150) に見えている点
    const Point before = vp.imageToScreen().apply(imagePoint);
    CHECK(nearly(before.x, 200));
    CHECK(nearly(before.y, 150));

    vp.zoomAt(2.0f, {200, 150});
    CHECK(nearly(vp.zoom(), 1.0f));
    CHECK(!vp.fitMode());
    const Point after = vp.imageToScreen().apply(imagePoint);
    CHECK(nearly(after.x, 200));
    CHECK(nearly(after.y, 150));

    // パンは画像端がウィンドウ内に収まる範囲へクランプされる
    vp.panBy(100000, 100000);
    const Point corner = vp.imageToScreen().apply({0, 0});  // 画像左上
    CHECK(nearly(corner.x, 0));  // 限界までパンすると左上が (0,0) に一致
    CHECK(nearly(corner.y, 0));

    // 画像がウィンドウより小さい場合はパン不可(中央固定)
    vp.setImage({100, 100});
    vp.actualSize();
    vp.panBy(500, 500);
    const Point smallCenter = vp.imageToScreen().apply({50, 50});
    CHECK(nearly(smallCenter.x, 400));
    CHECK(nearly(smallCenter.y, 300));
}

void testKeymap() {
    const Keymap km = Keymap::defaults();
    CHECK(km.find({KeyCode::Right}) == Command::NextImage);
    CHECK(km.find({KeyCode::Right, true}) == Command::PanRight);  // Ctrl+Right
    CHECK(km.find({KeyCode{'R'}}) == Command::RotateCW);
    CHECK(km.find({KeyCode{'R'}, false, true}) == Command::RotateCCW);  // Shift+R
    CHECK(km.find({KeyCode{'Z'}}) == Command::None);

    auto chord = Keymap::parseChord("Ctrl+O");
    CHECK(chord && chord->key == KeyCode{'O'} && chord->ctrl && !chord->shift);
    chord = Keymap::parseChord("shift+r");
    CHECK(chord && chord->key == KeyCode{'R'} && chord->shift);
    chord = Keymap::parseChord("F11");
    CHECK(chord && chord->key == KeyCode::F11);
    CHECK(km.find({KeyCode{'C'}, true}) == Command::CopyImage);         // Ctrl+C
    CHECK(km.find({KeyCode{'C'}, true, true}) == Command::CopyPath);    // Shift+Ctrl+C
    chord = Keymap::parseChord("+");
    CHECK(chord && chord->key == KeyCode::Plus);
    chord = Keymap::parseChord("Ctrl++");
    CHECK(chord && chord->key == KeyCode::Plus && chord->ctrl);
    CHECK(!Keymap::parseChord("foo"));
    CHECK(!Keymap::parseChord(""));

    // ini による上書き: 記述したコマンドの既存バインドは置き換わる
    Keymap custom = Keymap::defaults();
    custom.applyConfig({{"next", "N, Tab"}});
    CHECK(custom.find({KeyCode{'N'}}) == Command::NextImage);
    CHECK(custom.find({KeyCode::Tab}) == Command::NextImage);
    CHECK(custom.find({KeyCode::Right}) == Command::None);       // 置き換え済み
    CHECK(custom.find({KeyCode::Left}) == Command::PrevImage);   // 他コマンドは無傷
}

void testConfig() {
    const Config cfg = Config::parse(
        "# コメント\n"
        "[View]\n"
        "Background = #FF8000\n"
        "fit_upscale = true\n"
        "prefetch_radius = 3\n"
        "; これもコメント\n"
        "[keys]\n"
        "next = N\n"
        "broken_line_without_equal\n");
    CHECK(cfg.getColorRGB("view", "background", 0) == 0xFF8000u);
    CHECK(cfg.getBool("view", "fit_upscale", false) == true);
    CHECK(cfg.getInt("view", "prefetch_radius", 2) == 3);
    CHECK(cfg.get("keys", "next") == "N");
    CHECK(cfg.get("keys", "missing", "def") == "def");
    CHECK(cfg.getInt("view", "missing", 42) == 42);
    CHECK(cfg.getColorRGB("view", "fit_upscale", 7) == 7u);  // 色として不正 → 既定値
}

void testImageList() {
    ImageList list;
    CHECK(list.empty());

    list.set({L"C:/pics/1.png", L"C:/pics/2.png", L"C:/pics/10.png"}, L"C:/PICS/2.PNG");
    CHECK(list.size() == 3);
    CHECK(list.index() == 1);  // 大文字小文字を無視して一致

    CHECK(list.next());
    CHECK(list.index() == 2);
    CHECK(!list.next());  // 末尾で停止
    CHECK(list.first());
    CHECK(list.index() == 0);
    CHECK(!list.prev());  // 先頭で停止
    CHECK(list.last());
    CHECK(list.index() == 2);

    list.set({L"a.png", L"b.png", L"c.png", L"d.png", L"e.png"}, L"c.png");
    const auto order = list.prefetchOrder(2);
    CHECK(order.size() == 4);
    CHECK(order[0].filename() == L"d.png");  // +1 が最優先
    CHECK(order[1].filename() == L"b.png");
    CHECK(order[2].filename() == L"e.png");
    CHECK(order[3].filename() == L"a.png");
}

// 1x1 のダミー画像を返すテスト用デコーダ。"fail" を含むパスは失敗させる
class FakeDecoder final : public IImageDecoder {
public:
    std::shared_ptr<DecodedImage> decode(const std::filesystem::path& path) override {
        if (path.wstring().find(L"fail") != std::wstring::npos) return nullptr;
        auto image = std::make_shared<DecodedImage>();
        image->width = 1;
        image->height = 1;
        image->pixels = {0, 0, 0, 255};
        return image;
    }
};

void testImageCache() {
    FakeDecoder decoder;
    ImageCache cache(decoder);

    std::mutex mutex;
    std::condition_variable cv;
    int decodedCount = 0;
    cache.setOnDecoded([&](const std::filesystem::path&) {
        std::lock_guard lock(mutex);
        ++decodedCount;
        cv.notify_all();
    });

    cache.requestNow(L"ok.png");
    cache.requestNow(L"fail.png");
    {
        std::unique_lock lock(mutex);
        const bool done = cv.wait_for(lock, std::chrono::seconds(5),
                                      [&] { return decodedCount >= 2; });
        CHECK(done);
    }

    bool failed = false;
    const auto image = cache.tryGet(L"ok.png", &failed);
    CHECK(image != nullptr);
    CHECK(!failed);
    CHECK(image->width == 1);

    const auto none = cache.tryGet(L"fail.png", &failed);
    CHECK(none == nullptr);
    CHECK(failed);

    CHECK(cache.tryGet(L"never_requested.png") == nullptr);
}

class FakeHost final : public IAppHost {
public:
    void requestRedraw() override {}
    void setTitle(const std::wstring&) override {}
    void setFullscreen(bool enabled) override { fullscreen = enabled; }
    bool isFullscreen() const override { return fullscreen; }
    std::optional<std::filesystem::path> showOpenDialog() override { return std::nullopt; }
    void quit() override {}

    bool fullscreen = false;
};

class FakeFileSystem final : public IFileSystem {
public:
    std::vector<std::filesystem::path> listImages(const std::filesystem::path&) override {
        return files;
    }

    std::vector<std::filesystem::path> files;
};

class FakeClipboard final : public IClipboard {
public:
    bool setImage(const DecodedImage& image) override {
        ++imageCount;
        lastWidth = image.width;
        return true;
    }
    bool setText(const std::wstring& text) override {
        lastText = text;
        return true;
    }

    int imageCount = 0;
    uint32_t lastWidth = 0;
    std::wstring lastText;
};

void testAppClipboard() {
    FakeDecoder decoder;
    ImageCache cache(decoder);
    FakeHost host;
    FakeFileSystem fileSystem;
    FakeClipboard clipboard;
    App app(host, fileSystem, cache, clipboard);

    // 画像を開いていない状態では何もコピーされない
    app.execute(Command::CopyImage);
    app.execute(Command::CopyPath);
    CHECK(clipboard.imageCount == 0);
    CHECK(clipboard.lastText.empty());

    // デコード完了を同期して待てるようにしてから画像を開く
    std::mutex mutex;
    std::condition_variable cv;
    bool decoded = false;
    cache.setOnDecoded([&](const std::filesystem::path&) {
        std::lock_guard lock(mutex);
        decoded = true;
        cv.notify_all();
    });
    const std::filesystem::path path = L"C:/pics/a.png";
    fileSystem.files = {path};
    app.openPath(path);
    {
        std::unique_lock lock(mutex);
        CHECK(cv.wait_for(lock, std::chrono::seconds(5), [&] { return decoded; }));
    }
    app.onDecodeCompleted();  // 本来は UI スレッドへの PostMessage 経由
    CHECK(app.currentImage() != nullptr);

    app.execute(Command::CopyImage);
    CHECK(clipboard.imageCount == 1);
    CHECK(clipboard.lastWidth == 1);  // FakeDecoder は 1x1 を返す

    app.execute(Command::CopyPath);
    CHECK(clipboard.lastText == path.wstring());
}

} // namespace

int main() {
    testMatrix();
    testViewportFit();
    testViewportZoomAt();
    testKeymap();
    testConfig();
    testImageList();
    testImageCache();
    testAppClipboard();

    if (g_failures == 0) {
        std::cout << "all tests passed\n";
        return 0;
    }
    std::cout << g_failures << " check(s) failed\n";
    return 1;
}
