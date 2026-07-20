// core 層の単体テスト。フレームワーク不使用の軽量 CHECK マクロで検証する。
// 実行: build/<preset>/tests/core_tests.exe (ctest からも起動される)

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <format>
#include <iostream>
#include <mutex>

#include "core/annotation_edit.h"
#include "core/app.h"
#include "core/config.h"
#include "core/dib.h"
#include "core/edit.h"
#include "core/geometry.h"
#include "core/image_cache.h"
#include "core/image_list.h"
#include "core/keymap.h"
#include "core/pixel_convert.h"
#include "core/str_util.h"
#include "core/text_edit.h"
#include "core/unicode.h"
#include "core/version.h"
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

    // 逆変換との往復で元に戻る
    const Matrix3x2 t =
        Matrix3x2::translation(5, -3) * Matrix3x2::scale(2) * Matrix3x2::rotation90(1);
    const Point back = t.inverted().apply(t.apply({12, 34}));
    CHECK(nearly(back.x, 12));
    CHECK(nearly(back.y, 34));
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

    // screenToImage は imageToScreen の逆変換
    const Point roundTrip = vp.screenToImage().apply(after);
    CHECK(nearly(roundTrip.x, imagePoint.x));
    CHECK(nearly(roundTrip.y, imagePoint.y));

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
    CHECK(km.find({KeyCode::Down}) == Command::NextImage);
    CHECK(km.find({KeyCode::Up}) == Command::PrevImage);
    CHECK(km.find({KeyCode{'W'}, true}) == Command::Quit);  // Ctrl+W
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
    CHECK(km.find({KeyCode{'V'}, true}) == Command::PasteImage);        // Ctrl+V
    CHECK(km.find({KeyCode{'S'}, true}) == Command::SaveImageAs);       // Ctrl+S
    CHECK(km.find({KeyCode{'B'}}) == Command::ToggleStatusBar);
    CHECK(km.find({KeyCode{'B'}, true}) == Command::ToggleSidebar);     // Ctrl+B
    CHECK(commandFromName("paste") == Command::PasteImage);
    CHECK(commandFromName("save_as") == Command::SaveImageAs);
    CHECK(commandFromName("sidebar") == Command::ToggleSidebar);
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

    list.set({"C:/pics/1.png", "C:/pics/2.png", "C:/pics/10.png"}, "C:/PICS/2.PNG");
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

    list.set({"a.png", "b.png", "c.png", "d.png", "e.png"}, "c.png");
    CHECK(list.at(3).filename() == "d.png");
    CHECK(list.jumpTo(4));
    CHECK(list.index() == 4);
    CHECK(!list.jumpTo(4));  // 同じ位置は false
    CHECK(!list.jumpTo(5));  // 範囲外は無視
    CHECK(list.index() == 4);
    CHECK(list.jumpTo(2));

    const auto order = list.prefetchOrder(2);
    CHECK(order.size() == 4);
    CHECK(order[0].filename() == "d.png");  // +1 が最優先
    CHECK(order[1].filename() == "b.png");
    CHECK(order[2].filename() == "e.png");
    CHECK(order[3].filename() == "a.png");
}

// ---- DIB 変換 (クリップボード貼り付け用) ----

void putU16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x));
    v.push_back(static_cast<uint8_t>(x >> 8));
}

void putU32(std::vector<uint8_t>& v, uint32_t x) {
    putU16(v, static_cast<uint16_t>(x));
    putU16(v, static_cast<uint16_t>(x >> 16));
}

// 40 バイトの BITMAPINFOHEADER を組み立てる
std::vector<uint8_t> dibHeader(int32_t width, int32_t height, uint16_t bitCount,
                               uint32_t compression, uint32_t clrUsed = 0) {
    std::vector<uint8_t> v;
    putU32(v, 40);
    putU32(v, static_cast<uint32_t>(width));
    putU32(v, static_cast<uint32_t>(height));
    putU16(v, 1);  // planes
    putU16(v, bitCount);
    putU32(v, compression);
    putU32(v, 0);  // sizeImage
    putU32(v, 0);  // xppm
    putU32(v, 0);  // yppm
    putU32(v, clrUsed);
    putU32(v, 0);  // clrImportant
    return v;
}

// 124 バイトの BITMAPV5HEADER (BI_BITFIELDS、BGRA マスク付き)
std::vector<uint8_t> dibV5Header(int32_t width, int32_t height) {
    std::vector<uint8_t> v = dibHeader(width, height, 32, 3 /*BI_BITFIELDS*/);
    v[0] = 124;  // bV5Size
    putU32(v, 0x00FF0000);  // red
    putU32(v, 0x0000FF00);  // green
    putU32(v, 0x000000FF);  // blue
    putU32(v, 0xFF000000);  // alpha
    v.resize(124, 0);
    return v;
}

// 出力 (PBGRA) の (x, y) が期待値どおりか
bool pixelIs(const DecodedImage& img, uint32_t x, uint32_t y, uint8_t b, uint8_t g, uint8_t r,
             uint8_t a) {
    const uint8_t* p = img.pixels.data() + (static_cast<size_t>(y) * img.width + x) * 4;
    return p[0] == b && p[1] == g && p[2] == r && p[3] == a;
}

void testDib() {
    // 32bpp BI_RGB 2x2 ボトムアップ。第4バイトは未定義なので不透明扱い
    {
        auto d = dibHeader(2, 2, 32, 0);
        // 格納順は下の行から: (0,1)=青, (1,1)=白 / (0,0)=赤, (1,0)=緑(第4バイトにゴミ)
        putU32(d, 0x000000FF);  // 青 (XXRRGGBB リトルエンディアン格納 → B,G,R,X)
        putU32(d, 0x00FFFFFF);  // 白
        putU32(d, 0x00FF0000);  // 赤
        putU32(d, 0x7F00FF00);  // 緑 + ゴミアルファ 0x7F
        const auto img = imageFromDib(d.data(), d.size());
        CHECK(img && img->width == 2 && img->height == 2);
        CHECK(pixelIs(*img, 0, 0, 0, 0, 255, 255));      // 赤
        CHECK(pixelIs(*img, 1, 0, 0, 255, 0, 255));      // 緑 (ゴミアルファは無視)
        CHECK(pixelIs(*img, 0, 1, 255, 0, 0, 255));      // 青
        CHECK(pixelIs(*img, 1, 1, 255, 255, 255, 255));  // 白

        // トップダウン (高さ負) は格納順のまま
        auto t = dibHeader(2, -2, 32, 0);
        putU32(t, 0x000000FF);
        putU32(t, 0x00FFFFFF);
        putU32(t, 0x00FF0000);
        putU32(t, 0x7F00FF00);
        const auto timg = imageFromDib(t.data(), t.size());
        CHECK(timg && pixelIs(*timg, 0, 0, 255, 0, 0, 255));  // 先頭行が上 → (0,0)=青
        CHECK(pixelIs(*timg, 1, 1, 0, 255, 0, 255));
    }

    // CF_DIBV5: アルファマスク付き。ストレート → 事前乗算される
    {
        auto d = dibV5Header(1, 1);
        putU32(d, 0x800000FF);  // 青、アルファ 128
        const auto img = imageFromDib(d.data(), d.size());
        CHECK(img && pixelIs(*img, 0, 0, 128, 0, 0, 128));  // (255*128+127)/255 = 128
    }

    // アルファマスク付きなのに全ピクセル a=0 → 不透明として救済
    {
        auto d = dibV5Header(2, 1);
        putU32(d, 0x001E140A);  // (B,G,R) = (10,20,30), a=0
        putU32(d, 0x00000000);
        const auto img = imageFromDib(d.data(), d.size());
        CHECK(img && pixelIs(*img, 0, 0, 10, 20, 30, 255));
        CHECK(pixelIs(*img, 1, 0, 0, 0, 0, 255));
    }

    // 24bpp: 行が 4 バイト境界にパディングされる (幅3 → 9 バイト + 3 パディング)
    {
        auto d = dibHeader(3, 2, 24, 0);
        const uint8_t rows[2][12] = {
            {1, 2, 3, 4, 5, 6, 7, 8, 9, 0, 0, 0},           // 下の行
            {10, 20, 30, 40, 50, 60, 70, 80, 90, 0, 0, 0},  // 上の行
        };
        for (const auto& row : rows) d.insert(d.end(), row, row + 12);
        const auto img = imageFromDib(d.data(), d.size());
        CHECK(img && img->width == 3 && img->height == 2);
        CHECK(pixelIs(*img, 0, 0, 10, 20, 30, 255));
        CHECK(pixelIs(*img, 2, 0, 70, 80, 90, 255));
        CHECK(pixelIs(*img, 2, 1, 7, 8, 9, 255));
    }

    // 16bpp BI_BITFIELDS (565): マスク 3 個が 40 バイトヘッダの直後に続く
    {
        auto d = dibHeader(2, 1, 16, 3);
        putU32(d, 0xF800);  // red
        putU32(d, 0x07E0);  // green
        putU32(d, 0x001F);  // blue
        putU16(d, 0xF800);  // 赤ピクセル
        putU16(d, 0x07E0);  // 緑ピクセル
        const auto img = imageFromDib(d.data(), d.size());
        CHECK(img && pixelIs(*img, 0, 0, 0, 0, 255, 255));
        CHECK(pixelIs(*img, 1, 0, 0, 255, 0, 255));
    }

    // 16bpp BI_RGB は 555 固定
    {
        auto d = dibHeader(1, 1, 16, 0);
        putU16(d, 0x7C00);  // 赤 (5bit 最大値 → 255 にスケール)
        putU16(d, 0);       // パディング
        const auto img = imageFromDib(d.data(), d.size());
        CHECK(img && pixelIs(*img, 0, 0, 0, 0, 255, 255));
    }

    // 8bpp パレット
    {
        auto d = dibHeader(2, 1, 8, 0, 2);
        putU32(d, 0x00030201);  // パレット0: B=1, G=2, R=3
        putU32(d, 0x0096C8FA);  // パレット1: B=250, G=200, R=150
        d.push_back(1);         // (0,0) → パレット1
        d.push_back(0);         // (1,0) → パレット0
        putU16(d, 0);           // 行パディング
        const auto img = imageFromDib(d.data(), d.size());
        CHECK(img && pixelIs(*img, 0, 0, 250, 200, 150, 255));
        CHECK(pixelIs(*img, 1, 0, 1, 2, 3, 255));
    }

    // 不正データは nullptr
    {
        auto d = dibHeader(2, 2, 32, 0);
        for (int i = 0; i < 4; ++i) putU32(d, 0);
        CHECK(imageFromDib(d.data(), d.size() - 1) == nullptr);  // ピクセル不足
        CHECK(imageFromDib(d.data(), 39) == nullptr);            // ヘッダ不足
        CHECK(imageFromDib(nullptr, 100) == nullptr);

        const auto reject = [](std::vector<uint8_t> header) {
            header.resize(4096, 0);  // ピクセル領域は十分に確保した上でヘッダだけ不正にする
            return imageFromDib(header.data(), header.size()) == nullptr;
        };
        CHECK(reject(dibHeader(2, 2, 4, 0)));        // 4bpp 非対応
        CHECK(reject(dibHeader(2, 2, 8, 1)));        // RLE 非対応
        CHECK(reject(dibHeader(0, 2, 32, 0)));       // 幅 0
        CHECK(reject(dibHeader(100000, 1, 32, 0)));  // 巨大
    }
}

void testPixelConvert() {
    DecodedImage img;
    img.width = 3;
    img.height = 1;
    // 事前乗算 (128,64,32, a=128) / 不透明 (10,20,30) / 完全透明
    img.pixels = {128, 64, 32, 128, 10, 20, 30, 255, 0, 0, 0, 0};

    const auto straight = toStraightBGRA(img);
    CHECK(straight.size() == 12);
    CHECK(straight[0] == 255 && straight[1] == 128 && straight[2] == 64 && straight[3] == 128);
    CHECK(straight[4] == 10 && straight[5] == 20 && straight[6] == 30 && straight[7] == 255);
    CHECK(straight[8] == 0 && straight[11] == 0);

    const auto opaque = toOpaqueBGR(img);
    CHECK(opaque.size() == 9);
    // 白合成: premult + (255 - a)
    CHECK(opaque[0] == 255 && opaque[1] == 191 && opaque[2] == 159);
    CHECK(opaque[3] == 10 && opaque[4] == 20 && opaque[5] == 30);
    CHECK(opaque[6] == 255 && opaque[7] == 255 && opaque[8] == 255);  // 透明 → 白
}

// 1x1 のダミー画像を返すテスト用デコーダ。"fail" を含むパスは失敗させる
class FakeDecoder final : public IImageDecoder {
public:
    std::shared_ptr<DecodedImage> decode(const std::filesystem::path& path) override {
        if (pathToUtf8(path).find("fail") != std::string::npos) return nullptr;
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

    cache.requestNow("ok.png");
    cache.requestNow("fail.png");
    {
        std::unique_lock lock(mutex);
        const bool done = cv.wait_for(lock, std::chrono::seconds(5),
                                      [&] { return decodedCount >= 2; });
        CHECK(done);
    }

    bool failed = false;
    const auto image = cache.tryGet("ok.png", &failed);
    CHECK(image != nullptr);
    CHECK(!failed);
    CHECK(image->width == 1);

    const auto none = cache.tryGet("fail.png", &failed);
    CHECK(none == nullptr);
    CHECK(failed);

    CHECK(cache.tryGet("never_requested.png") == nullptr);
}

class FakeHost final : public IAppHost {
public:
    void requestRedraw() override {}
    void setTitle(const std::string& title) override { lastTitle = title; }
    void setFullscreen(bool enabled) override { fullscreen = enabled; }
    bool isFullscreen() const override { return fullscreen; }
    std::optional<std::filesystem::path> showOpenDialog() override { return std::nullopt; }
    std::optional<std::filesystem::path> showSaveDialog(
        const std::string& defaultFileName) override {
        ++saveDialogCount;
        lastDefaultName = defaultFileName;
        return savePath;
    }
    std::optional<size_t> showContextMenu(const std::vector<MenuItem>& items, Point) override {
        ++menuCount;
        lastMenuItems = items;
        // キューがあれば先頭から順に応答する(設定→編集の連続選択のテスト用)
        if (!menuQueue.empty()) {
            const size_t choice = menuQueue.front();
            menuQueue.pop_front();
            return choice;
        }
        return menuChoice;
    }
    void setTextEditing(bool active, Point caretScreenPos, float caretHeightPx) override {
        textEditing = active;
        ++textEditingCalls;
        lastCaretPos = caretScreenPos;
        lastCaretHeight = caretHeightPx;
    }
    std::optional<uint32_t> showColorPicker(uint32_t initialRGB) override {
        ++colorPickerCount;
        lastColorPickerInitial = initialRGB;
        return colorChoice;
    }
    void startTimer(unsigned milliseconds) override { lastTimerMs = milliseconds; }
    void quit() override {}

    bool fullscreen = false;
    unsigned lastTimerMs = 0;
    std::string lastTitle;
    std::optional<std::filesystem::path> savePath;  // 保存ダイアログの応答 (nullopt = キャンセル)
    int saveDialogCount = 0;
    std::string lastDefaultName;
    std::optional<size_t> menuChoice;  // メニューの応答 (nullopt = キャンセル)
    std::deque<size_t> menuQueue;      // 空でなければ menuChoice より優先
    int menuCount = 0;
    std::vector<MenuItem> lastMenuItems;
    bool textEditing = false;    // setTextEditing が最後に通知した状態
    int textEditingCalls = 0;    // setTextEditing の呼び出し回数
    Point lastCaretPos;          // 最後に通知されたキャレット位置
    float lastCaretHeight = 0;   // 最後に通知されたキャレット高さ
    std::optional<uint32_t> colorChoice;    // 色ダイアログの応答 (nullopt = キャンセル)
    uint32_t lastColorPickerInitial = 0;
    int colorPickerCount = 0;
};

// 選択可能な末端項目(separator とサブメニュー親を除く)の数。index の対応確認用
size_t countMenuLeaves(const std::vector<MenuItem>& items) {
    size_t count = 0;
    for (const MenuItem& item : items) {
        if (item.separator) continue;
        if (!item.children.empty()) {
            count += countMenuLeaves(item.children);
        } else {
            ++count;
        }
    }
    return count;
}

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
        lastPixels = image.pixels;
        return true;
    }
    bool setText(const std::string& text) override {
        lastText = text;
        return true;
    }
    std::shared_ptr<DecodedImage> getImage() override { return pasteImage; }
    std::string getText() override { return pasteText; }

    int imageCount = 0;
    std::string pasteText;  // getText の応答
    uint32_t lastWidth = 0;
    std::vector<uint8_t> lastPixels;
    std::string lastText;
    std::shared_ptr<DecodedImage> pasteImage;  // getImage の応答 (nullptr = 画像なし)
};

class FakeEncoder final : public IImageEncoder {
public:
    bool encode(const DecodedImage& image, const std::filesystem::path& path) override {
        ++encodeCount;
        lastWidth = image.width;
        lastPath = path;
        return ok;
    }

    bool ok = true;
    int encodeCount = 0;
    uint32_t lastWidth = 0;
    std::filesystem::path lastPath;
};

// 指定サイズの不透明単色 overlay を返すテスト用ラスタライザ
class FakeAnnotationRasterizer final : public IAnnotationRasterizer {
public:
    AnnotationOverlay rasterize(const AnnotationSpec& spec) override {
        ++rasterizeCount;
        lastSpec = spec;
        if (!ok) return {};
        auto image = std::make_shared<DecodedImage>();
        image->width = overlayWidth;
        image->height = overlayHeight;
        image->pixels.resize(static_cast<size_t>(overlayWidth) * overlayHeight * 4);
        for (size_t i = 0; i < image->pixels.size(); i += 4) {
            image->pixels[i] = 0;        // B
            image->pixels[i + 1] = 0;    // G
            image->pixels[i + 2] = 255;  // R
            image->pixels[i + 3] = 255;  // A
        }
        return {std::move(image), overlayX, overlayY};
    }

    // テキスト計測は「1 行・1 文字 kCharWidth px・行高 kLineHeight px」の単純な模型にする
    static constexpr float kCharWidth = 10.0f;
    static constexpr float kLineHeight = 20.0f;

    TextCaretMetrics caretMetrics(const AnnotationSpec& spec, size_t utf16Offset) override {
        const size_t length = utf8ToUtf16Offset(spec.text, spec.text.size());
        return {static_cast<float>(std::min(utf16Offset, length)) * kCharWidth, 0.0f,
                kLineHeight};
    }

    size_t hitTestTextOffset(const AnnotationSpec& spec, float localX, float) override {
        const size_t length = utf8ToUtf16Offset(spec.text, spec.text.size());
        if (localX <= 0) return 0;
        return std::min(static_cast<size_t>(localX / kCharWidth + 0.5f), length);
    }

    std::vector<TextRangeRect> selectionRects(const AnnotationSpec&, size_t utf16Begin,
                                              size_t utf16End) override {
        if (utf16End <= utf16Begin) return {};
        return {{static_cast<float>(utf16Begin) * kCharWidth, 0.0f,
                 static_cast<float>(utf16End) * kCharWidth, kLineHeight}};
    }

    bool ok = true;
    int rasterizeCount = 0;
    uint32_t overlayWidth = 1;
    uint32_t overlayHeight = 1;
    int overlayX = 0;
    int overlayY = 0;
    AnnotationSpec lastSpec;
};

void testAppClipboard() {
    FakeDecoder decoder;
    ImageCache cache(decoder);
    FakeHost host;
    FakeFileSystem fileSystem;
    FakeClipboard clipboard;
    FakeEncoder encoder;
    FakeAnnotationRasterizer rasterizer;
    App app(host, fileSystem, cache, clipboard, encoder, rasterizer);

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
    const std::filesystem::path path = "C:/pics/a.png";
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
    CHECK(clipboard.lastText == pathToUtf8(path));
}

void testAppStatusBar() {
    FakeDecoder decoder;
    ImageCache cache(decoder);
    FakeHost host;
    FakeFileSystem fileSystem;
    FakeClipboard clipboard;
    FakeEncoder encoder;
    FakeAnnotationRasterizer rasterizer;
    App app(host, fileSystem, cache, clipboard, encoder, rasterizer);
    app.onResize(800, 600);

    // 画像なし: バーは表示されるが左右とも空
    StatusBarView bar = app.statusBar();
    CHECK(bar.visible);
    CHECK(bar.height > 0);
    CHECK(bar.leftText.empty());
    CHECK(bar.rightText.empty());

    // 画像 (FakeDecoder の 1x1 黒) を開く
    std::mutex mutex;
    std::condition_variable cv;
    bool decoded = false;
    cache.setOnDecoded([&](const std::filesystem::path&) {
        std::lock_guard lock(mutex);
        decoded = true;
        cv.notify_all();
    });
    const std::filesystem::path path = "C:/pics/black.png";
    fileSystem.files = {path};
    app.openPath(path);
    {
        std::unique_lock lock(mutex);
        CHECK(cv.wait_for(lock, std::chrono::seconds(5), [&] { return decoded; }));
    }
    app.onDecodeCompleted();
    CHECK(app.currentImage() != nullptr);
    CHECK(app.statusBar().leftText == "1 x 1 px");

    // 1x1 画像はビューポート 800x(600-26) の中央 (400, 287) に等倍表示される。
    // その位置にカーソルを置くとピクセル (0,0) の座標と色が出る
    app.onMouseMove({400, 287});
    CHECK(app.statusBar().rightText == "(0, 0)  #000000  RGB(0, 0, 0)");

    // ステータスバー上・画像外では表示しない
    app.onMouseMove({400, 590});
    CHECK(app.statusBar().rightText.empty());
    app.onMouseMove({400, 287});
    CHECK(!app.statusBar().rightText.empty());
    app.onMouseLeave();
    CHECK(app.statusBar().rightText.empty());

    // コピーで通知が出て、タイマー満了で画像情報表示に戻る
    app.execute(Command::CopyImage);
    CHECK(app.statusBar().leftText == "画像をクリップボードにコピーしました");
    CHECK(host.lastTimerMs == 3000);
    app.execute(Command::CopyPath);
    CHECK(app.statusBar().leftText == "パスをコピーしました: " + pathToUtf8(path));
    app.onTimer();
    CHECK(app.statusBar().leftText == "1 x 1 px");

    // トグルとフルスクリーンで非表示になる
    app.execute(Command::ToggleStatusBar);
    CHECK(!app.statusBar().visible);
    app.execute(Command::ToggleStatusBar);
    CHECK(app.statusBar().visible);
    host.fullscreen = true;
    CHECK(!app.statusBar().visible);
    host.fullscreen = false;

    // タイトルバーには常にバージョンと git SHA-1 が付く
    const std::string appName = std::format("Blinker v{} ({})", kAppVersion, kAppGitSha);
    CHECK(host.lastTitle.ends_with(" - " + appName));
}

void testAppPasteSave() {
    FakeDecoder decoder;
    ImageCache cache(decoder);
    FakeHost host;
    FakeFileSystem fileSystem;
    FakeClipboard clipboard;
    FakeEncoder encoder;
    FakeAnnotationRasterizer rasterizer;
    App app(host, fileSystem, cache, clipboard, encoder, rasterizer);
    app.onResize(800, 600);

    // 画像なしでの保存: ダイアログは開かずメッセージ
    app.execute(Command::SaveImageAs);
    CHECK(host.saveDialogCount == 0);
    CHECK(app.statusBar().leftText == "保存する画像がありません");

    // クリップボードが空のときの貼り付け
    app.execute(Command::PasteImage);
    CHECK(app.currentImage() == nullptr);
    CHECK(app.statusBar().leftText == "クリップボードに画像がありません");

    // フォルダの画像 (FakeDecoder の 1x1) を開く
    std::mutex mutex;
    std::condition_variable cv;
    bool decoded = false;
    cache.setOnDecoded([&](const std::filesystem::path&) {
        std::lock_guard lock(mutex);
        decoded = true;
        cv.notify_all();
    });
    const std::filesystem::path path = "C:/pics/a.png";
    fileSystem.files = {path};
    app.openPath(path);
    {
        std::unique_lock lock(mutex);
        CHECK(cv.wait_for(lock, std::chrono::seconds(5), [&] { return decoded; }));
    }
    app.onDecodeCompleted();
    CHECK(app.currentImage() && app.currentImage()->width == 1);

    // 2x1 のクリップボード画像を貼り付け
    auto pasted = std::make_shared<DecodedImage>();
    pasted->width = 2;
    pasted->height = 1;
    pasted->pixels = {0, 0, 255, 255, 0, 255, 0, 255};
    clipboard.pasteImage = pasted;
    app.onTimer();  // 直前の通知メッセージを消しておく
    app.execute(Command::PasteImage);
    CHECK(app.currentImage() && app.currentImage()->width == 2);
    CHECK(host.lastTitle.find("(クリップボード)") == 0);
    CHECK(app.statusBar().leftText == "2 x 1 px");

    // デコード完了通知が来ても貼り付け画像は上書きされない
    app.onDecodeCompleted();
    CHECK(app.currentImage()->width == 2);

    // 貼り付け表示中はパスのコピーを拒否(一覧のパスとは無関係のため)
    app.execute(Command::CopyPath);
    CHECK(app.statusBar().leftText == "コピーするパスがありません");

    // 貼り付け画像の保存: 既定名は「クリップボード.png」
    host.savePath = std::filesystem::path("C:/out/pasted.png");
    app.execute(Command::SaveImageAs);
    CHECK(host.lastDefaultName == "クリップボード.png");
    CHECK(encoder.lastPath == std::filesystem::path("C:/out/pasted.png"));
    CHECK(encoder.lastWidth == 2);
    CHECK(app.statusBar().leftText == "保存しました: C:/out/pasted.png");

    // 次へ移動でフォルダ一覧の表示に戻る(1枚しかなくても)
    app.execute(Command::NextImage);
    CHECK(app.currentImage() && app.currentImage()->width == 1);
    CHECK(host.lastTitle.find("a.png") != std::string::npos);

    // 通常画像の保存: 既定名は元ファイル名の .png 置き換え
    host.savePath = std::filesystem::path("C:/out/copy.jpg");
    app.execute(Command::SaveImageAs);
    CHECK(host.lastDefaultName == "a.png");
    CHECK(encoder.lastWidth == 1);
    CHECK(app.statusBar().leftText == "保存しました: C:/out/copy.jpg");

    // ダイアログのキャンセル: エンコードもメッセージも発生しない
    app.onTimer();  // 前のメッセージを消す
    host.savePath.reset();
    const int encodeCountBefore = encoder.encodeCount;
    app.execute(Command::SaveImageAs);
    CHECK(encoder.encodeCount == encodeCountBefore);
    CHECK(app.statusBar().leftText == "1 x 1 px");

    // 保存失敗
    encoder.ok = false;
    host.savePath = std::filesystem::path("C:/out/x.png");
    app.execute(Command::SaveImageAs);
    CHECK(app.statusBar().leftText == "保存に失敗しました: C:/out/x.png");
}

void testAppSidebar() {
    FakeDecoder decoder;
    ImageCache cache(decoder);
    FakeHost host;
    FakeFileSystem fileSystem;
    FakeClipboard clipboard;
    FakeEncoder encoder;
    FakeAnnotationRasterizer rasterizer;
    App app(host, fileSystem, cache, clipboard, encoder, rasterizer);
    app.onResize(800, 600);

    // 既定では非表示
    CHECK(!app.sidebar().visible);

    // 30 枚の一覧 (f01..f30) の 10 枚目を開く
    std::mutex mutex;
    std::condition_variable cv;
    bool decoded = false;
    cache.setOnDecoded([&](const std::filesystem::path&) {
        std::lock_guard lock(mutex);
        decoded = true;
        cv.notify_all();
    });
    for (int i = 1; i <= 30; ++i) {
        fileSystem.files.push_back(std::format("C:/pics/f{:02}.png", i));
    }
    app.openPath(fileSystem.files[9]);
    {
        std::unique_lock lock(mutex);
        CHECK(cv.wait_for(lock, std::chrono::seconds(5), [&] { return decoded; }));
    }
    app.onDecodeCompleted();
    CHECK(app.currentImage() != nullptr);

    // Ctrl+B 相当でトグル表示
    app.execute(Command::ToggleSidebar);
    SidebarView sb = app.sidebar();
    CHECK(sb.visible);
    CHECK(nearly(sb.width, 220));
    CHECK(nearly(sb.height, 574));          // 600 - ステータスバー26
    CHECK(nearly(sb.itemHeight, 24));
    CHECK(nearly(sb.contentHeight, 720));   // 30 * 24
    CHECK(nearly(sb.scrollOffset, 0));      // 10 枚目 (y=216..240) は視界内
    CHECK(sb.items.size() == 25);           // 可視範囲のみ (574/24 + 2)
    CHECK(sb.items[0].text == "f01.png");
    CHECK(sb.items[9].current);
    CHECK(!sb.items[0].current);

    // 画像はサイドバーの右側の領域 (580x574) の中央に描画される
    const Point center = app.imageToScreen().apply({0.5f, 0.5f});
    CHECK(nearly(center.x, 220 + 290));
    CHECK(nearly(center.y, 287));

    // 末尾へ移動すると現在項目が見えるまでスクロールする
    app.execute(Command::LastImage);
    sb = app.sidebar();
    CHECK(nearly(sb.scrollOffset, 720 - 574));
    CHECK(sb.items.back().current);

    // サイドバー上のホイールはスクロール (1ノッチ = 3項目) でズームしない
    const float zoomBefore = app.zoom();
    app.onWheel(1.0f, {100, 300});
    sb = app.sidebar();
    CHECK(nearly(sb.scrollOffset, 146 - 72));
    CHECK(nearly(app.zoom(), zoomBefore));
    app.onWheel(-100.0f, {100, 300});  // 大きく下へ → 末尾でクランプ
    CHECK(nearly(app.sidebar().scrollOffset, 146));
    app.onWheel(100.0f, {100, 300});   // 大きく上へ → 先頭でクランプ
    CHECK(nearly(app.sidebar().scrollOffset, 0));

    // ビューポート上のホイールは従来どおりズーム
    app.onWheel(1.0f, {500, 300});
    CHECK(app.zoom() > zoomBefore);
    app.execute(Command::ZoomFit);

    // クリックでジャンプ: scroll=0 で y=100 → index 4 (f05.png)
    CHECK(app.onMouseDown({100, 100}));
    CHECK(host.lastTitle.find("f05.png") == 0);
    // ビューポート上のクリックは消費しない(パン開始に回す)
    CHECK(!app.onMouseDown({500, 300}));
    // サイドバー幅内でもステータスバーの高さでは消費のみ(ジャンプしない)
    CHECK(app.onMouseDown({100, 590}));
    CHECK(host.lastTitle.find("f05.png") == 0);

    // f05 のデコード完了を待って表示を確定させる(以降のチェックを決定的にする)
    {
        std::unique_lock lock(mutex);
        CHECK(cv.wait_for(lock, std::chrono::seconds(5),
                          [&] { return cache.tryGet(fileSystem.files[4]) != nullptr; }));
    }
    app.onDecodeCompleted();
    CHECK(app.currentImage() && app.currentImage()->width == 1);

    // 貼り付け表示中に現在項目をクリック → 一覧表示へ戻る
    auto pasted = std::make_shared<DecodedImage>();
    pasted->width = 2;
    pasted->height = 1;
    pasted->pixels = {0, 0, 0, 255, 0, 0, 0, 255};
    clipboard.pasteImage = pasted;
    app.execute(Command::PasteImage);
    CHECK(host.lastTitle.find("(クリップボード)") == 0);
    CHECK(app.onMouseDown({100, 100}));  // index 4 = 現在項目
    CHECK(host.lastTitle.find("f05.png") == 0);
    CHECK(app.currentImage() && app.currentImage()->width == 1);

    // フルスクリーン中は非表示
    host.fullscreen = true;
    CHECK(!app.sidebar().visible);
    host.fullscreen = false;

    // ini で初期表示と幅を指定できる
    app.applyConfig(Config::parse("[view]\nsidebar = true\nsidebar_width = 300\n"));
    sb = app.sidebar();
    CHECK(sb.visible);
    CHECK(nearly(sb.width, 300));
}

void testEditFunctions() {
    // 4x2、B チャンネル = 通し番号*10 の画像
    DecodedImage src;
    src.width = 4;
    src.height = 2;
    src.pixels.resize(4 * 2 * 4);
    for (uint32_t i = 0; i < 8; ++i) {
        const uint8_t v = static_cast<uint8_t>(i * 10);
        src.pixels[i * 4 + 0] = v;
        src.pixels[i * 4 + 1] = static_cast<uint8_t>(v + 1);
        src.pixels[i * 4 + 2] = static_cast<uint8_t>(v + 2);
        src.pixels[i * 4 + 3] = 255;
    }

    // 中央 2x2 の切り出し
    const auto cropped = cropImage(src, {1, 0, 2, 2});
    CHECK(cropped && cropped->width == 2 && cropped->height == 2);
    CHECK(cropped->pixels[0] == 10);      // (0,0) = 元 (1,0)
    CHECK(cropped->pixels[1 * 4] == 20);  // (1,0) = 元 (2,0)
    CHECK(cropped->pixels[2 * 4] == 50);  // (0,1) = 元 (1,1)

    // 画像外へはみ出す指定はクランプされる
    const auto clamped = cropImage(src, {-5, -5, 100, 100});
    CHECK(clamped && clamped->width == 4 && clamped->height == 2);
    CHECK(clamped->pixels == src.pixels);

    // 有効領域が残らなければ nullptr
    CHECK(cropImage(src, {4, 0, 2, 2}) == nullptr);
    CHECK(cropImage(src, {1, 1, 0, 0}) == nullptr);

    // 半透明 (a=128) の事前乗算 over 合成
    DecodedImage dst;
    dst.width = 2;
    dst.height = 2;
    dst.pixels.assign(2 * 2 * 4, 100);
    for (int i = 0; i < 4; ++i) dst.pixels[i * 4 + 3] = 255;
    DecodedImage overlay;
    overlay.width = 1;
    overlay.height = 1;
    overlay.pixels = {0, 0, 128, 128};  // 事前乗算済みの半透明赤
    blendOverlay(dst, overlay, 1, 0);
    CHECK(dst.pixels[1 * 4 + 0] == 50);   // B: 0 + 100*127/255
    CHECK(dst.pixels[1 * 4 + 2] == 178);  // R: 128 + 100*127/255
    CHECK(dst.pixels[1 * 4 + 3] == 255);  // A: 128 + 255*127/255
    CHECK(dst.pixels[0] == 100);          // (0,0) は変化しない

    // 不透明 overlay は上書き。はみ出しはクリップされる
    DecodedImage red;
    red.width = 2;
    red.height = 2;
    red.pixels.resize(2 * 2 * 4);
    for (int i = 0; i < 4; ++i) {
        red.pixels[i * 4 + 2] = 255;
        red.pixels[i * 4 + 3] = 255;
    }
    blendOverlay(dst, red, -1, -1);  // overlay の右下 1 ピクセルだけが (0,0) に載る
    CHECK(dst.pixels[2] == 255);              // (0,0) は赤
    CHECK(dst.pixels[(1 * 2 + 1) * 4] == 100);  // (1,1) は変化しない
}

void testAnnotationGeometry() {
    AnnotationSpec rect;
    rect.kind = AnnotationSpec::Kind::Rect;
    rect.p1 = {10, 20};
    rect.p2 = {0, 0};  // 順不同でも正規化される
    rect.strokeWidth = 2;

    const BoundsF b = annotationBounds(rect);
    CHECK(nearly(b.minX, 0) && nearly(b.minY, 0) && nearly(b.maxX, 10) && nearly(b.maxY, 20));
    const Point c = annotationCenter(rect);
    CHECK(nearly(c.x, 5) && nearly(c.y, 10));

    // 矩形: 輪郭の近傍のみヒット(内部は外れてパンに使える)。reach = 太さ/2 + 許容 = 2
    CHECK(hitTestAnnotation(rect, {0, 0}, 1));       // 角
    CHECK(hitTestAnnotation(rect, {-1.5f, 10}, 1));  // 左辺のすぐ外側
    CHECK(!hitTestAnnotation(rect, {5, 10}, 1));     // 中心は外れ
    CHECK(!hitTestAnnotation(rect, {-3, 10}, 1));    // 届かない距離

    // 90° 回転: 幅10x高20 が中心 (5,10) 周りで横長になる
    rect.angleDeg = 90;
    CHECK(hitTestAnnotation(rect, {c.x + 10, c.y}, 1));   // 回転後の右辺(元の下辺)
    CHECK(!hitTestAnnotation(rect, {c.x, c.y + 10}, 1));  // 回転後の高さは半分の5まで

    // rotatedCorners: 元の TL (0,0) は中心周り 90° 回転で (15,5) へ
    const auto corners = rotatedCorners(rect);
    CHECK(nearly(corners[0].x, 15) && nearly(corners[0].y, 5));
    CHECK(nearly(corners[2].x, -5) && nearly(corners[2].y, 15));  // 元の BR (10,20)

    // 楕円: 輪郭上のみ
    AnnotationSpec ellipse;
    ellipse.kind = AnnotationSpec::Kind::Ellipse;
    ellipse.p1 = {0, 0};
    ellipse.p2 = {20, 10};
    ellipse.strokeWidth = 2;
    CHECK(hitTestAnnotation(ellipse, {20, 5}, 1));   // 右端の輪郭
    CHECK(hitTestAnnotation(ellipse, {10, 0}, 1));   // 上端の輪郭
    CHECK(!hitTestAnnotation(ellipse, {10, 5}, 1));  // 中心は外れ

    // 直線・矢印: 線分への距離で判定
    AnnotationSpec line;
    line.kind = AnnotationSpec::Kind::Line;
    line.p1 = {0, 0};
    line.p2 = {10, 0};
    line.strokeWidth = 2;
    CHECK(hitTestAnnotation(line, {5, 1.5f}, 1));
    CHECK(!hitTestAnnotation(line, {5, 4}, 1));
    CHECK(!hitTestAnnotation(line, {14, 0}, 1));  // 端点の先

    // テキスト: バウンディングボックス内部
    AnnotationSpec text;
    text.kind = AnnotationSpec::Kind::Text;
    text.p1 = {0, 0};
    text.p2 = {30, 10};
    CHECK(hitTestAnnotation(text, {15, 5}, 1));
    CHECK(!hitTestAnnotation(text, {15, 12}, 1));

    // 重なりは最前面(末尾)が勝つ
    const std::vector<AnnotationSpec> specs{text, line};
    CHECK(hitTestAnnotations(specs, {5, 0.5f}, 1) == std::optional<size_t>(1));
    CHECK(hitTestAnnotations(specs, {25, 8}, 1) == std::optional<size_t>(0));
    CHECK(!hitTestAnnotations(specs, {100, 100}, 1));

    AnnotationSpec moved = line;
    translateAnnotation(moved, 5, 7);
    CHECK(nearly(moved.p1.x, 5) && nearly(moved.p1.y, 7) && nearly(moved.p2.x, 15));

    // 回転ハンドル: 無回転なら上辺中央の真上
    AnnotationSpec plain;
    plain.p1 = {0, 0};
    plain.p2 = {10, 10};
    const Point handle = rotationHandlePos(plain, Matrix3x2::identity(), 20);
    CHECK(nearly(handle.x, 5) && nearly(handle.y, -20));

    CHECK(nearly(angleDegFrom({0, 0}, {10, 0}), 0));
    CHECK(nearly(angleDegFrom({0, 0}, {0, 10}), 90));  // Y 下向きで時計回り
    CHECK(nearly(snapAngleDeg(47, 15), 45));
    CHECK(nearly(snapAngleDeg(83, 15), 90));
    CHECK(nearly(normalizeAngleDeg(-90), 270));
    CHECK(nearly(normalizeAngleDeg(370), 10));

    // サイズ変更ハンドル: 図形は8個、テキストは上下辺なしの6個、直線・矢印は端点2個
    CHECK(resizeHandlePositions(plain).size() == 8);
    CHECK(resizeHandlePositions(text).size() == 6);
    CHECK(resizeHandlePositions(line).size() == 2);

    // 無回転の BR ドラッグは p2 だけ動く。辺ハンドルは一方向のみ
    AnnotationSpec r2 = resizeAnnotation(plain, ResizeHandle::BottomRight, {14, 16}, false);
    CHECK(nearly(r2.p1.x, 0) && nearly(r2.p1.y, 0));
    CHECK(nearly(r2.p2.x, 14) && nearly(r2.p2.y, 16));
    r2 = resizeAnnotation(plain, ResizeHandle::Left, {-4, 100}, false);
    CHECK(nearly(r2.p1.x, -4) && nearly(r2.p1.y, 0));
    CHECK(nearly(r2.p2.x, 10) && nearly(r2.p2.y, 10));

    // 反対側の辺は越えない(最小1px)
    r2 = resizeAnnotation(plain, ResizeHandle::Right, {-100, 5}, false);
    CHECK(nearly(r2.p2.x - r2.p1.x, 1));

    // Shift(縦横比維持)は大きい方の倍率に合わせる
    r2 = resizeAnnotation(plain, ResizeHandle::BottomRight, {20, 15}, true);
    CHECK(nearly(r2.p2.x, 20) && nearly(r2.p2.y, 20));

    // 回転中のリサイズはアンカー(反対側の角)の見た目の位置が変わらない
    AnnotationSpec rot = plain;
    rot.angleDeg = 30;
    const Point tlBefore = rotatedCorners(rot)[0];
    const AnnotationSpec rotResized =
        resizeAnnotation(rot, ResizeHandle::BottomRight, {20, 18}, false);
    CHECK(nearly(rotResized.angleDeg, 30));
    const Point tlAfter = rotatedCorners(rotResized)[0];
    CHECK(nearly(tlAfter.x, tlBefore.x, 0.01f) && nearly(tlAfter.y, tlBefore.y, 0.01f));

    // 端点ドラッグ (Line/Arrow): 他端の見た目の位置は固定される
    AnnotationSpec rline;
    rline.kind = AnnotationSpec::Kind::Line;
    rline.p1 = {0, 0};
    rline.p2 = {10, 0};
    rline.angleDeg = 90;  // 見た目は (5,-5)-(5,5)
    const AnnotationSpec dragged = resizeAnnotation(rline, ResizeHandle::P2, {5, 9}, false);
    CHECK(nearly(dragged.p1.x, -2) && nearly(dragged.p1.y, 2));
    CHECK(nearly(dragged.p2.x, 12) && nearly(dragged.p2.y, 2));
    const auto endpoints = resizeHandlePositions(dragged);
    CHECK(nearly(endpoints[0].pos.x, 5) && nearly(endpoints[0].pos.y, -5));  // P1 は不動
    CHECK(nearly(endpoints[1].pos.x, 5) && nearly(endpoints[1].pos.y, 9));
}

void testAppAnnotationObjects() {
    FakeDecoder decoder;
    ImageCache cache(decoder);
    FakeHost host;
    FakeFileSystem fileSystem;
    FakeClipboard clipboard;
    FakeEncoder encoder;
    FakeAnnotationRasterizer rasterizer;
    App app(host, fileSystem, cache, clipboard, encoder, rasterizer);
    app.onResize(800, 600);

    // 8x8 画像を貼り付け、矩形注釈 (0,0)-(4,4) を追加(画像左上はスクリーン (396,283))
    auto source = std::make_shared<DecodedImage>();
    source->width = 8;
    source->height = 8;
    source->pixels.resize(8 * 8 * 4);
    clipboard.pasteImage = source;
    app.execute(Command::PasteImage);
    host.menuChoice = 1;  // 矩形
    app.onRightDragStart({396, 283});
    app.onRightDragEnd({400, 287});
    CHECK(app.annotations().specs->size() == 1);
    CHECK(app.annotations().selected.has_value());

    // Escape はまず選択解除に使われる
    app.execute(Command::Escape);
    CHECK(!app.annotations().selected.has_value());

    // 注釈の輪郭をクリックすると選択して移動ドラッグが始まる(クリックを消費しパンしない)
    CHECK(app.onMouseDown({396, 283}));  // 画像 (0,0) = 矩形の角
    CHECK(app.annotations().selected == std::optional<size_t>(0));
    app.onMouseMove({398, 285});  // +2px 移動
    {
        const AnnotationsView view = app.annotations();
        const AnnotationSpec& spec = view.specs->front();
        CHECK(nearly(spec.p1.x, 2) && nearly(spec.p1.y, 2));
        CHECK(nearly(spec.p2.x, 6) && nearly(spec.p2.y, 6));
    }
    app.onMouseMove({397, 284});  // ドラッグ継続(+1px に戻す)
    CHECK(nearly(app.annotations().specs->front().p1.x, 1));
    app.onMouseUp();

    // ドラッグ1回の undo は1段。取り消しで元の位置に戻り選択は解除される
    app.execute(Command::Undo);
    CHECK(nearly(app.annotations().specs->front().p1.x, 0));
    CHECK(!app.annotations().selected.has_value());

    // 何もない場所のクリックは消費しない(選択解除してパンに回る)
    CHECK(app.onMouseDown({396, 283}));
    app.onMouseUp();
    CHECK(!app.onMouseDown({600, 450}));
    CHECK(!app.annotations().selected.has_value());
    app.onMouseUp();

    // サイズ変更: 選択中の右下ハンドル(画像 (4,4) = スクリーン (400,287))をドラッグ
    CHECK(app.onMouseDown({396, 283}));  // まず本体クリックで選択
    app.onMouseUp();
    CHECK(app.onMouseDown({400, 287}));  // 右下ハンドルを掴む
    app.onMouseMove({402, 289});
    {
        const AnnotationsView view = app.annotations();
        const AnnotationSpec& spec = view.specs->front();
        CHECK(nearly(spec.p1.x, 0) && nearly(spec.p1.y, 0));
        CHECK(nearly(spec.p2.x, 6) && nearly(spec.p2.y, 6));
    }
    app.onMouseUp();
    app.execute(Command::Undo);  // リサイズ1回で undo 1段
    CHECK(nearly(app.annotations().specs->front().p2.x, 4));
    CHECK(!app.annotations().selected.has_value());

    // 回転ハンドル(枠上辺中央の 20px 上)のドラッグで回転する
    CHECK(app.onMouseDown({396, 283}));  // 選択し直す
    app.onMouseUp();
    CHECK(app.onMouseDown({398, 263}));  // 中心 (398,285)、ハンドル (398,263)
    app.onMouseMove({420, 285});         // 中心の真右 → 90°
    CHECK(nearly(app.annotations().specs->front().angleDeg, 90));
    app.onMouseMove({421, 287}, true);   // Shift で 15° 単位にスナップ
    CHECK(nearly(std::fmod(app.annotations().specs->front().angleDeg, 15.0f), 0));
    app.onMouseUp();
    app.execute(Command::Undo);
    CHECK(nearly(app.annotations().specs->front().angleDeg, 0));

    // 右クリック(ドラッグ閾値未満)でオブジェクトメニュー。末端 index (図形):
    // 0 削除, 1-8 回転 {0,15,30,45,90,135,180,270}, 9-15 太さ {1,2,3,5,8,12,20}, 16 色
    host.menuChoice = 5;  // 90°
    app.onRightDragStart({396, 283});
    app.onRightDragEnd({397, 283});
    CHECK(countMenuLeaves(host.lastMenuItems) == 17);
    CHECK(nearly(app.annotations().specs->front().angleDeg, 90));

    host.menuChoice = 13;  // 太さ 8px
    app.onRightDragStart({396, 283});
    app.onRightDragEnd({396, 283});
    CHECK(nearly(app.annotations().specs->front().strokeWidth, 8));

    host.colorChoice = 0x123456;
    host.menuChoice = 16;  // 色の変更
    app.onRightDragStart({396, 283});
    app.onRightDragEnd({396, 283});
    CHECK(app.annotations().specs->front().colorRGB == 0x123456);

    // テキスト注釈はその場で入力して追加し、ダブルクリックで再編集する
    host.menuChoice = 5;  // テキスト
    rasterizer.overlayWidth = 24;
    rasterizer.overlayHeight = 44;  // 実測境界 20x40(リサイズテストでハンドルを離すため縦長)
    const int measureCount = rasterizer.rasterizeCount;
    app.onRightDragStart({401, 286});  // 画像 (5,3)
    app.onRightDragEnd({404, 290});    // 閾値以上のドラッグで編集メニューを出す
    CHECK(app.isTextEditing());        // 空のテキストボックスができ、その場で入力できる
    CHECK(host.textEditing);           // host には編集開始が伝わる (IME 有効化)
    CHECK(rasterizer.rasterizeCount == measureCount);  // 内容が空の間は実測しない
    app.insertText("元のテキスト");
    app.onKey({KeyCode::Escape});      // Esc で確定
    CHECK(!app.isTextEditing());
    CHECK(!host.textEditing);
    CHECK(rasterizer.rasterizeCount == measureCount + 2);  // 高さ合わせ + 確定時の実測
    CHECK(app.annotations().specs->size() == 2);
    CHECK(app.annotations().specs->back().text == "元のテキスト");

    CHECK(app.onDoubleClick({402, 288}));  // テキスト上のダブルクリックで再編集を始める
    CHECK(app.isTextEditing());
    app.insertText("更新後");  // ダブルクリックで語が選択されているため置き換わる
    app.onKey({KeyCode::Escape});
    CHECK(app.annotations().specs->back().text == "更新後");
    CHECK(!app.onDoubleClick({600, 450}));  // テキスト以外の場所では何もしない

    // Delete で選択中の注釈を削除。選択なしはメッセージのみ。undo で復活する
    app.execute(Command::DeleteAnnotation);  // ダブルクリックで選択済み
    CHECK(app.annotations().specs->size() == 1);
    app.execute(Command::DeleteAnnotation);
    CHECK(app.statusBar().leftText == "削除する注釈がありません");
    app.onTimer();
    app.execute(Command::Undo);
    CHECK(app.annotations().specs->size() == 2);
    CHECK(app.annotations().specs->back().text == "更新後");

    // トリミング後も注釈はオブジェクトのまま維持され、座標が平行移動する
    host.menuChoice = 0;  // トリミング
    app.onRightDragStart({398, 285});  // 画像 (2,2)
    app.onRightDragEnd({402, 289});    // 画像 (6,6) → 4x4 に切り出し
    CHECK(app.currentImage()->width == 4);
    CHECK(app.annotations().specs->size() == 2);
    CHECK(nearly(app.annotations().specs->front().p1.x, -2));  // (0,0) → (-2,-2)
    app.execute(Command::Undo);
    CHECK(app.currentImage()->width == 8);
    CHECK(nearly(app.annotations().specs->front().p1.x, 0));

    // 保存は注釈を合成した画像を出力する(注釈の数だけラスタライズされる)
    const int rasterizeBeforeSave = rasterizer.rasterizeCount;
    host.savePath = std::filesystem::path("C:/out/annotated.png");
    app.execute(Command::SaveImageAs);
    CHECK(encoder.lastWidth == 8);
    CHECK(rasterizer.rasterizeCount == rasterizeBeforeSave + 2);

    // テキストのリサイズ: 右ハンドルで折り返し幅を変え、確定時に実寸へ揃える。
    // テキストは (5,3)-(25,43)、右ハンドルは画像 (25,23) = スクリーン (421,306)
    CHECK(app.onMouseDown({402, 288}));  // 本体クリックで選択(最前面のテキスト)
    app.onMouseUp();
    CHECK(app.annotations().selected == std::optional<size_t>(1));
    CHECK(app.onMouseDown({421, 306}));
    const int beforeResize = rasterizer.rasterizeCount;
    app.onMouseMove({431, 306});
    CHECK(nearly(app.annotations().specs->back().p2.x, 35));  // ドラッグ中は掴んだ幅のまま
    CHECK(rasterizer.rasterizeCount == beforeResize);
    app.onMouseUp();  // 確定時に折り返し後の実寸を測り直す
    CHECK(rasterizer.rasterizeCount == beforeResize + 1);
    CHECK(nearly(app.annotations().specs->back().p2.x, 25));
    CHECK(nearly(app.annotations().specs->back().p2.y, 43));
}

void testAppEdit() {
    FakeDecoder decoder;
    ImageCache cache(decoder);
    FakeHost host;
    FakeFileSystem fileSystem;
    FakeClipboard clipboard;
    FakeEncoder encoder;
    FakeAnnotationRasterizer rasterizer;
    App app(host, fileSystem, cache, clipboard, encoder, rasterizer);
    app.onResize(800, 600);

    // 画像がないときは選択を開始しない
    app.onRightDragStart({400, 300});
    CHECK(!app.selection().visible);
    app.onRightDragEnd({500, 400});
    CHECK(host.menuCount == 0);

    // 8x8 の不透明青画像を貼り付けて編集対象にする。
    // ビューポート 800x574 の中央に等倍表示され、画像左上はスクリーン (396, 283)
    auto source = std::make_shared<DecodedImage>();
    source->width = 8;
    source->height = 8;
    source->pixels.resize(8 * 8 * 4);
    for (size_t i = 0; i < source->pixels.size(); i += 4) {
        source->pixels[i] = 255;      // B
        source->pixels[i + 3] = 255;  // A
    }
    clipboard.pasteImage = source;
    app.execute(Command::PasteImage);
    CHECK(app.currentImage() && app.currentImage()->width == 8);

    // 閾値未満の右ドラッグ(ただの右クリック)は無視
    app.onRightDragStart({400, 300});
    app.onRightDragEnd({402, 301});
    CHECK(host.menuCount == 0);

    // ドラッグ中はラバーバンドが出て、Escape で解除できる
    app.onRightDragStart({396, 283});
    app.onRightDragMove({400, 287});
    const SelectionView sel = app.selection();
    CHECK(sel.visible);
    CHECK(nearly(sel.p1.x, 396) && nearly(sel.p1.y, 283));
    CHECK(nearly(sel.p2.x, 400) && nearly(sel.p2.y, 287));
    app.execute(Command::Escape);
    CHECK(!app.selection().visible);

    // メニューをキャンセルすると何も起きない
    host.menuChoice = std::nullopt;
    app.onRightDragStart({396, 283});
    app.onRightDragEnd({400, 287});
    CHECK(host.menuCount == 1);
    // 末端項目: 編集6種 + 太さ7 + 文字サイズ7 + 色1 = 21(回転角度はオブジェクト側へ移動)
    CHECK(countMenuLeaves(host.lastMenuItems) == 21);
    CHECK(app.currentImage()->width == 8);
    CHECK(rasterizer.rasterizeCount == 0);

    // トリミング: 画像座標 (0,0)-(4,4) → 4x4
    host.menuChoice = 0;
    app.onRightDragStart({396, 283});
    app.onRightDragEnd({400, 287});
    CHECK(app.currentImage()->width == 4 && app.currentImage()->height == 4);
    CHECK(app.statusBar().leftText == "4 x 4 px");
    CHECK(host.lastTitle.find("(編集済み)") != std::string::npos);
    CHECK(!app.selection().visible);

    // Undo で元に戻る。履歴が空ならメッセージ
    app.execute(Command::Undo);
    CHECK(app.currentImage()->width == 8);
    CHECK(host.lastTitle.find("(編集済み)") == std::string::npos);
    app.execute(Command::Undo);
    CHECK(app.statusBar().leftText == "取り消す編集はありません");
    app.onTimer();

    // 矩形: 画像へは焼き込まず注釈オブジェクトとして追加され、追加直後は選択状態になる
    host.menuChoice = 1;
    app.onRightDragStart({396, 283});
    app.onRightDragEnd({400, 287});
    CHECK(rasterizer.rasterizeCount == 0);  // 図形の追加ではラスタライズしない
    {
        const AnnotationsView view = app.annotations();
        CHECK(view.specs && view.specs->size() == 1);
        const AnnotationSpec& spec = view.specs->front();
        CHECK(spec.kind == AnnotationSpec::Kind::Rect);
        CHECK(nearly(spec.p1.x, 0) && nearly(spec.p2.x, 4));
        CHECK(nearly(spec.strokeWidth, 3));  // 等倍表示なので画面基準の値のまま
        CHECK(spec.colorRGB == 0xFF3B30);
        CHECK(nearly(spec.angleDeg, 0));
        CHECK(view.selected && *view.selected == 0);
    }
    CHECK(host.lastTitle.find("(編集済み)") != std::string::npos);
    CHECK(app.currentImage()->pixels[(1 * 8 + 1) * 4 + 2] == 0);  // 画像自体は無変更

    // コピーは注釈を合成した画像になる (2x2 赤 overlay が (1,1) へ)
    rasterizer.overlayWidth = 2;
    rasterizer.overlayHeight = 2;
    rasterizer.overlayX = 1;
    rasterizer.overlayY = 1;
    app.execute(Command::CopyImage);
    CHECK(rasterizer.rasterizeCount == 1);
    CHECK(clipboard.lastWidth == 8);
    CHECK(clipboard.lastPixels[(1 * 8 + 1) * 4 + 2] == 255);  // (1,1) は赤
    CHECK(clipboard.lastPixels[2] == 0);                      // (0,0) は青のまま
    CHECK(app.currentImage()->pixels[(1 * 8 + 1) * 4 + 2] == 0);  // 元画像は無変更
    // 貼り付け元(キャッシュ相当)の画像も書き換えられていない
    CHECK(source->pixels[(1 * 8 + 1) * 4 + 2] == 0);

    // テキスト: 空のまま確定すると追加されない。入力があれば実測して追加される
    host.menuChoice = 5;
    app.onRightDragStart({396, 283});
    app.onRightDragEnd({400, 287});
    CHECK(app.isTextEditing());
    CHECK(app.annotations().specs->size() == 2);  // 編集中は空のテキストボックスが入る
    app.onKey({KeyCode::Escape});
    CHECK(rasterizer.rasterizeCount == 1);        // 空なので実測は走らない
    CHECK(app.annotations().specs->size() == 1);  // 空の箱は残さない
    rasterizer.overlayWidth = 24;   // 実測境界: 24-4 x 12-4 (テキスト余白 2px x 両側)
    rasterizer.overlayHeight = 12;
    app.onRightDragStart({396, 283});
    app.onRightDragEnd({400, 287});
    app.insertText("メモ");
    app.onKey({KeyCode::Escape});
    CHECK(rasterizer.rasterizeCount == 3);  // 高さ合わせ + 確定時の実測
    CHECK(rasterizer.lastSpec.kind == AnnotationSpec::Kind::Text);
    CHECK(rasterizer.lastSpec.text == "メモ");
    {
        const AnnotationsView view = app.annotations();
        const AnnotationSpec& text = view.specs->back();
        CHECK(nearly(text.fontSize, 18));  // 既定値 (等倍表示)
        CHECK(nearly(text.p1.x, 0) && nearly(text.p1.y, 0));
        CHECK(nearly(text.p2.x, 20) && nearly(text.p2.y, 8));  // 実測境界が p2 に入る
    }

    // 設定変更を選ぶとメニューが再表示され、選択領域を保ったまま続けて編集できる。
    // 末端 index: 0-5 編集, 6-12 太さ {1,2,3,5,8,12,20}, 13-19 文字サイズ
    // {12,14,18,24,36,48,72}, 20 色
    host.menuChoice = std::nullopt;
    host.menuQueue = {10 /*太さ8px*/, 1 /*矩形*/};
    app.onRightDragStart({396, 283});
    app.onRightDragEnd({400, 287});
    CHECK(host.menuQueue.empty());
    CHECK(nearly(app.annotations().specs->back().strokeWidth, 8));

    // 文字サイズ 24px + 複数行テキスト。変更済みの設定も引き継がれる
    host.menuQueue = {16 /*文字24px*/, 5 /*テキスト*/};
    app.onRightDragStart({396, 283});
    app.onRightDragEnd({400, 287});
    app.insertText("1行目");
    app.onKey({KeyCode::Enter});  // 編集中の Enter は改行
    app.insertText("2行目");
    app.onKey({KeyCode::Escape});
    CHECK(app.annotations().specs->back().text == "1行目\n2行目");
    CHECK(nearly(app.annotations().specs->back().fontSize, 24));

    // 色の変更: ダイアログの結果が以降の編集に使われる。キャンセルなら元のまま
    host.colorChoice = 0x00CC66;
    host.menuQueue = {20 /*色*/, 4 /*直線*/};
    app.onRightDragStart({396, 283});
    app.onRightDragEnd({400, 287});
    CHECK(host.colorPickerCount == 1);
    CHECK(host.lastColorPickerInitial == 0xFF3B30);
    CHECK(app.annotations().specs->back().colorRGB == 0x00CC66);
    host.colorChoice = std::nullopt;
    host.menuQueue = {20 /*色 (キャンセル)*/, 4 /*直線*/};
    app.onRightDragStart({396, 283});
    app.onRightDragEnd({400, 287});
    CHECK(host.colorPickerCount == 2);
    CHECK(app.annotations().specs->back().colorRGB == 0x00CC66);

    // 設定変更だけしてメニューをキャンセル → 編集は適用されず設定は残る
    const size_t annotationCountBefore = app.annotations().specs->size();
    host.menuQueue = {8 /*太さ3px*/};  // 続くメニュー再表示は menuChoice = nullopt で閉じる
    app.onRightDragStart({396, 283});
    app.onRightDragEnd({400, 287});
    CHECK(app.annotations().specs->size() == annotationCountBefore);
    CHECK(!app.selection().visible);
    host.menuQueue = {1 /*矩形*/};
    app.onRightDragStart({396, 283});
    app.onRightDragEnd({400, 287});
    CHECK(nearly(app.annotations().specs->back().strokeWidth, 3));  // 3px に戻っている

    // 確定時の実測(ラスタライズ)失敗はメッセージを出す。入力済みの内容は残す
    rasterizer.ok = false;
    host.menuChoice = 5;
    const size_t countBeforeFail = app.annotations().specs->size();
    app.onRightDragStart({396, 283});
    app.onRightDragEnd({400, 287});
    app.insertText("失敗するテキスト");
    app.onKey({KeyCode::Escape});
    CHECK(app.statusBar().leftText == "描画に失敗しました");
    CHECK(app.annotations().specs->size() == countBeforeFail + 1);
    CHECK(app.annotations().specs->back().text == "失敗するテキスト");
    rasterizer.ok = true;
    app.onTimer();

    // 画像移動で編集(注釈含む)は破棄される(一覧が空なので表示もなくなる)
    app.execute(Command::NextImage);
    CHECK(app.statusBar().leftText == "編集を破棄しました");
    CHECK(app.currentImage() == nullptr);
    CHECK(app.annotations().specs->empty());
    CHECK(host.lastTitle == std::format("Blinker v{} ({})", kAppVersion, kAppGitSha));
    app.execute(Command::Undo);
    CHECK(app.statusBar().leftText == "取り消す編集はありません");
}

void testUnicode() {
    // ASCII・日本語・サロゲートペア(絵文字)の往復
    const std::string utf8 = "ABC 日本語テスト 😀";
    CHECK(wideToUtf8(utf8ToWide(utf8)) == utf8);
    CHECK(utf32ToUtf8(utf8ToUtf32(utf8)) == utf8);

    // コードポイント数の検証(😀 は1コードポイント)
    CHECK(utf8ToUtf32("😀").size() == 1);
    CHECK(utf8ToUtf32("日本語").size() == 3);

    // wchar_t のサイズに応じた表現(Windows: UTF-16 サロゲートペア、他: UTF-32)
    const std::wstring wide = utf8ToWide("😀");
    if constexpr (sizeof(wchar_t) == 2) {
        CHECK(wide.size() == 2);
        CHECK(wide[0] == wchar_t(0xD83D) && wide[1] == wchar_t(0xDE00));
    } else {
        CHECK(wide.size() == 1);
        CHECK(static_cast<char32_t>(wide[0]) == U'\U0001F600');
    }

    // 不正な UTF-8 は U+FFFD に置換される(例外なし・停止しない)
    CHECK(utf8ToUtf32("\x80").front() == char32_t(0xFFFD));          // 継続バイト単独
    CHECK(utf8ToUtf32("\xC2").front() == char32_t(0xFFFD));          // 途切れた2バイト列
    CHECK(utf8ToUtf32("\xED\xA0\x80").front() == char32_t(0xFFFD));  // サロゲート領域
    CHECK(utf8ToUtf32("\xC0\xAF").front() == char32_t(0xFFFD));      // 冗長表現 (overlong)

    // パス変換の往復(日本語ファイル名)
    const std::filesystem::path p = pathFromUtf8("フォルダ/画像 (1).png");
    CHECK(pathToUtf8Generic(p) == "フォルダ/画像 (1).png");
}

void testUtf16Offsets() {
    // "あ" は UTF-8 で 3 バイト / UTF-16 で 1 単位、"😀" は 4 バイト / 2 単位
    const std::string s = "aあ😀b";
    CHECK(utf8ToUtf16Offset(s, 0) == 0);
    CHECK(utf8ToUtf16Offset(s, 1) == 1);   // 'a' の後
    CHECK(utf8ToUtf16Offset(s, 4) == 2);   // "あ" の後
    CHECK(utf8ToUtf16Offset(s, 8) == 4);   // "😀" の後(サロゲートペアで +2)
    CHECK(utf8ToUtf16Offset(s, 9) == 5);   // 末尾
    CHECK(utf8ToUtf16Offset(s, 999) == 5); // 範囲外は末尾へ丸める

    CHECK(utf16ToUtf8Offset(s, 0) == 0);
    CHECK(utf16ToUtf8Offset(s, 1) == 1);
    CHECK(utf16ToUtf8Offset(s, 2) == 4);
    CHECK(utf16ToUtf8Offset(s, 4) == 8);
    CHECK(utf16ToUtf8Offset(s, 999) == s.size());
    // サロゲートペアの途中(3)を指したらペアの先頭へ切り下げる
    CHECK(utf16ToUtf8Offset(s, 3) == 4);
}

void testTextEditBuffer() {
    // 構築時のキャレットは末尾。挿入はキャレット位置に入る
    TextEditBuffer buf("あい");
    CHECK(buf.caret() == 6 && !buf.hasSelection());
    buf.insert("う");
    CHECK(buf.text() == "あいう" && buf.caret() == 9);

    // Backspace / Delete はマルチバイト 1 文字ずつ動く
    CHECK(buf.backspace());
    CHECK(buf.text() == "あい" && buf.caret() == 6);
    buf.setCaret(0, false);
    CHECK(!buf.backspace());  // 先頭では何も起きない
    CHECK(buf.deleteForward());
    CHECK(buf.text() == "い" && buf.caret() == 0);
    buf.setCaret(buf.text().size(), false);
    CHECK(!buf.deleteForward());  // 末尾では何も起きない

    // 左右移動もコードポイント単位。範囲外へは出ない
    TextEditBuffer moves("a😀b");
    moves.setCaret(0, false);
    moves.moveRight(false);
    CHECK(moves.caret() == 1);
    moves.moveRight(false);
    CHECK(moves.caret() == 5);  // 絵文字は 4 バイトまとめて飛ぶ
    moves.moveLeft(false);
    CHECK(moves.caret() == 1);
    moves.moveLeft(false);
    moves.moveLeft(false);
    CHECK(moves.caret() == 0);

    // Shift 付き移動で選択が伸び、選択中の挿入は置き換えになる
    TextEditBuffer sel("abcdef");
    sel.setCaret(1, false);
    sel.moveRight(true);
    sel.moveRight(true);
    CHECK(sel.hasSelection() && sel.selectedText() == "bc");
    CHECK(sel.selectionBegin() == 1 && sel.selectionEnd() == 3);
    sel.insert("X");
    CHECK(sel.text() == "aXdef" && !sel.hasSelection() && sel.caret() == 2);

    // 選択中の Backspace は選択を消すだけ(直前の文字は消さない)
    sel.selectAll();
    CHECK(sel.selectedText() == "aXdef");
    CHECK(sel.backspace());
    CHECK(sel.text().empty() && sel.caret() == 0);

    // 選択解除の左右移動は選択の端へ寄る
    TextEditBuffer collapse("abcdef");
    collapse.setCaret(1, false);
    collapse.setCaret(4, true);
    collapse.moveLeft(false);
    CHECK(collapse.caret() == 1 && !collapse.hasSelection());
    collapse.setCaret(1, false);
    collapse.setCaret(4, true);
    collapse.moveRight(false);
    CHECK(collapse.caret() == 4 && !collapse.hasSelection());

    // Home / End は論理行(LF 区切り)の端へ動く
    TextEditBuffer lines("abc\ndef");
    lines.setCaret(5, false);  // 2 行目の 'e' の後
    lines.moveLineStart(false);
    CHECK(lines.caret() == 4);
    lines.moveLineEnd(false);
    CHECK(lines.caret() == 7);
    lines.setCaret(1, false);
    lines.moveLineEnd(false);
    CHECK(lines.caret() == 3);  // LF の手前で止まる
    lines.moveLineStart(false);
    CHECK(lines.caret() == 0);

    // 語の選択: 空白・ASCII 英数字・それ以外の連なりをそれぞれ 1 語として扱う
    TextEditBuffer words("abc def");
    words.selectWordAt(1);
    CHECK(words.selectedText() == "abc");
    words.selectWordAt(3);
    CHECK(words.selectedText() == " ");
    words.selectWordAt(5);
    CHECK(words.selectedText() == "def");
    words.selectWordAt(999);  // 末尾クリックは直前の語
    CHECK(words.selectedText() == "def");
    TextEditBuffer jp("あいう abc");
    jp.selectWordAt(0);
    CHECK(jp.selectedText() == "あいう");  // 非 ASCII の連なりはまとめて 1 語
    TextEditBuffer empty("");
    empty.selectWordAt(0);
    CHECK(!empty.hasSelection());

    // 位置はコードポイント境界へ丸められる(マルチバイトの途中を指しても壊れない)
    TextEditBuffer clamp("あ");
    clamp.setCaret(2, false);
    CHECK(clamp.caret() == 0);
    clamp.setCaret(999, false);
    CHECK(clamp.caret() == 3);
}

void testAppTextEditing() {
    FakeDecoder decoder;
    ImageCache cache(decoder);
    FakeHost host;
    FakeFileSystem fs;
    FakeClipboard clipboard;
    FakeEncoder encoder;
    FakeAnnotationRasterizer rasterizer;
    App app(host, fs, cache, clipboard, encoder, rasterizer);

    app.onResize(800, 600);
    // 100x100 の画像を貼り付け、等倍表示にしてスクリーン座標を素直にする
    auto source = std::make_shared<DecodedImage>();
    source->width = 100;
    source->height = 100;
    source->pixels.resize(100 * 100 * 4);
    clipboard.pasteImage = source;
    app.execute(Command::PasteImage);
    app.execute(Command::ZoomActual);
    const Matrix3x2 toScreen = app.imageToScreen();
    // 実測境界 40x20(テキスト余白 2px x 両側)。枠を現実的な高さにしてクリック判定を安定させる
    rasterizer.overlayWidth = 44;
    rasterizer.overlayHeight = 24;
    const auto screenOf = [&toScreen](float x, float y) { return toScreen.apply({x, y}); };

    // テキストボックスを作って入力する
    host.menuChoice = 5;  // テキスト
    app.onRightDragStart(screenOf(10, 10));
    app.onRightDragEnd(screenOf(50, 30));
    CHECK(app.isTextEditing());
    app.insertText("abcdef");
    CHECK(app.annotations().specs->back().text == "abcdef");

    // キャレットは末尾。編集ビューにはキャレットの位置が入る(選択中は非表示)
    {
        const AnnotationsView view = app.annotations();
        CHECK(view.textEdit.active);
        CHECK(view.textEdit.index == 0);
        CHECK(view.textEdit.selectionRects.empty());
        // 枠の左端 (10) + 6 文字 x 10px
        CHECK(nearly(view.textEdit.caretTop.x, 70));
    }

    // 左矢印でキャレットが戻り、Shift + 左で選択が伸びる
    app.onKey({KeyCode::Left});
    app.onKey({KeyChord{KeyCode::Left, false, true, false}});
    {
        const AnnotationsView view = app.annotations();
        CHECK(!view.textEdit.caretVisible);  // 選択中はキャレットを出さない
        CHECK(view.textEdit.selectionRects.size() == 1);
        CHECK(nearly(view.textEdit.selectionRects[0].left, 10 + 40));
        CHECK(nearly(view.textEdit.selectionRects[0].right, 10 + 50));
    }

    // 選択範囲の切り取り → クリップボードへ渡り、本文からは消える
    app.onKey({KeyChord{static_cast<KeyCode>('X'), true, false, false}});
    CHECK(clipboard.lastText == "e");
    CHECK(app.annotations().specs->back().text == "abcdf");

    // 貼り付けはキャレット位置に入る
    clipboard.pasteText = "XY";
    app.onKey({KeyChord{static_cast<KeyCode>('V'), true, false, false}});
    CHECK(app.annotations().specs->back().text == "abcdXYf");

    // 全選択して置き換え
    app.onKey({KeyChord{static_cast<KeyCode>('A'), true, false, false}});
    app.insertText("Z");
    CHECK(app.annotations().specs->back().text == "Z");

    // 枠内クリックでキャレットが動き、ドラッグで範囲選択できる
    app.onKey({KeyChord{static_cast<KeyCode>('A'), true, false, false}});
    app.insertText("0123456789");
    app.onMouseDown(screenOf(10 + 25, 15));  // 枠内 2.5 文字目 → 境界 3
    CHECK(app.isTextEditing());
    app.onMouseMove(screenOf(10 + 55, 15));  // 5.5 文字目 → 境界 6
    app.onMouseUp();
    app.onKey({KeyChord{static_cast<KeyCode>('C'), true, false, false}});
    CHECK(clipboard.lastText == "345");  // [3,6) の 3 文字

    // 編集中の枠内では I ビームカーソルを出す(枠外・画像の別の場所では出さない)
    CHECK(app.wantsTextCursor(screenOf(30, 20)));   // 枠 (10,10)-(50,30) の内側
    CHECK(!app.wantsTextCursor(screenOf(90, 90)));  // 枠の外

    // IME の変換中文字列はキャレット位置にインライン表示され、確定するまで
    // 編集内容には入らない
    app.onKey({KeyCode::Home});  // 選択を解除してキャレットを先頭へ
    app.beginComposition();
    app.setComposition("にほんご", 12, 0, 6);  // キャレットは末尾、前半2文字が変換対象
    CHECK(app.isComposing());
    {
        const AnnotationsView view = app.annotations();
        // 表示は「変換中文字列 + 確定済みテキスト」。注釈の text も表示用になる
        CHECK(app.annotations().specs->back().text == "にほんご0123456789");
        CHECK(view.textEdit.compositionRects.size() == 1);
        CHECK(nearly(view.textEdit.compositionRects[0].left, 10 + 0));
        CHECK(nearly(view.textEdit.compositionRects[0].right, 10 + 40));  // 4 文字 x 10px
        // 変換対象の節(前半 2 文字)だけ太い下線になる
        CHECK(view.textEdit.compositionTargetRects.size() == 1);
        CHECK(nearly(view.textEdit.compositionTargetRects[0].right, 10 + 20));
        // キャレットは変換中文字列の末尾(4 文字目の後ろ)
        CHECK(nearly(view.textEdit.caretTop.x, 10 + 40));
    }

    // 変換を取り消すと表示も元に戻る(編集内容は変わっていない)
    app.clearComposition();
    CHECK(!app.isComposing());
    CHECK(app.annotations().specs->back().text == "0123456789");
    CHECK(app.annotations().textEdit.compositionRects.empty());

    // 確定すると変換中文字列が編集内容へ入る
    app.beginComposition();
    app.setComposition("にほん", 9, 0, 9);
    app.insertText("日本");  // IME の確定文字列
    CHECK(!app.isComposing());
    CHECK(app.annotations().specs->back().text == "日本0123456789");

    // 変換中のキー入力は IME が扱うので App は編集しない
    app.setComposition("あ", 3, 0, 0);  // キャレットは "日本" の直後
    app.onKey({KeyCode::Backspace});
    app.onKey({KeyCode::Delete});
    CHECK(app.annotations().specs->back().text == "日本あ0123456789");
    app.clearComposition();
    CHECK(app.annotations().specs->back().text == "日本0123456789");

    // 選択範囲があるときに変換を始めると選択は置き換えられる
    app.onKey({KeyChord{static_cast<KeyCode>('A'), true, false, false}});
    app.beginComposition();
    CHECK(app.annotations().specs->back().text.empty());
    app.insertText("再入力");
    CHECK(app.annotations().specs->back().text == "再入力");
    app.onKey({KeyChord{static_cast<KeyCode>('A'), true, false, false}});
    app.insertText("0123456789");  // 後続のテストのため元の内容へ戻す

    // 編集中はコマンドが暴発しない(次の画像へ移動しない・タイトルが変わらない)
    const std::string titleWhileEditing = host.lastTitle;
    app.onKey({KeyCode::Space});
    CHECK(app.isTextEditing());
    CHECK(host.lastTitle == titleWhileEditing);

    // キャレットは点滅する(タイマー通知で表示相が入れ替わる)
    app.onKey({KeyCode::End});  // 選択を解除してキャレットを出す
    const bool before = app.annotations().textEdit.caretVisible;
    app.onCaretBlink();
    CHECK(app.annotations().textEdit.caretVisible != before);

    // 枠の外のクリックで確定して編集が終わる
    app.onMouseDown(screenOf(90, 90));
    CHECK(!app.isTextEditing());
    CHECK(!host.textEditing);
    CHECK(app.annotations().specs->back().text == "0123456789");
    CHECK(!app.annotations().textEdit.active);
    // 確定後は枠の内側でも I ビームにしない(編集していないテキストは通常のカーソル)
    CHECK(!app.wantsTextCursor(screenOf(30, 20)));

    // 編集中の Ctrl+Z は編集開始前へ戻す(新規作成なら注釈ごと消える)
    const size_t countBeforeUndo = app.annotations().specs->size();
    host.menuChoice = 5;
    app.onRightDragStart(screenOf(10, 60));
    app.onRightDragEnd(screenOf(50, 80));
    app.insertText("捨てる");
    app.onKey({KeyChord{static_cast<KeyCode>('Z'), true, false, false}});
    CHECK(!app.isTextEditing());
    CHECK(app.annotations().specs->size() == countBeforeUndo);

    // 画像が切り替わると編集は破棄され、host にも終了が伝わる
    host.menuChoice = 5;
    app.onRightDragStart(screenOf(10, 60));
    app.onRightDragEnd(screenOf(50, 80));
    app.insertText("破棄される");
    CHECK(app.isTextEditing());
    fs.files = {"a.png"};
    app.openPath("a.png");
    app.onDecodeCompleted();
    CHECK(!app.isTextEditing());
    CHECK(!host.textEditing);
    CHECK(app.annotations().specs->empty());
}

void testNaturalCompare() {
    // 数字の連続は数値として比較(エクスプローラ相当)
    CHECK(naturalCompare("1.png", "2.png") < 0);
    CHECK(naturalCompare("2.png", "10.png") < 0);
    CHECK(naturalCompare("a2b", "a10b") < 0);
    CHECK(naturalCompare("img9.png", "img10.png") < 0);
    // 大文字小文字は無視(ASCII)
    CHECK(naturalCompare("ABC", "abc") == 0);
    CHECK(naturalCompare("a2", "B1") < 0);
    // 数値が同じなら先頭ゼロが少ない方が先
    CHECK(naturalCompare("1", "01") < 0);
    CHECK(naturalCompare("01.png", "01.png") == 0);
    // 前置詞・長さの違い
    CHECK(naturalCompare("abc", "abcd") < 0);
    CHECK(naturalCompare("", "a") < 0);
    // 非 ASCII (UTF-8) はバイト順 = コードポイント順
    CHECK(naturalCompare("あ", "い") < 0);
}

} // namespace

int main() {
    testUnicode();
    testUtf16Offsets();
    testNaturalCompare();
    testTextEditBuffer();
    testMatrix();
    testViewportFit();
    testViewportZoomAt();
    testKeymap();
    testConfig();
    testDib();
    testPixelConvert();
    testImageList();
    testImageCache();
    testAppClipboard();
    testAppStatusBar();
    testAppPasteSave();
    testAppSidebar();
    testEditFunctions();
    testAnnotationGeometry();
    testAppAnnotationObjects();
    testAppEdit();
    testAppTextEditing();

    if (g_failures == 0) {
        std::cout << "all tests passed\n";
        return 0;
    }
    std::cout << g_failures << " check(s) failed\n";
    return 1;
}
