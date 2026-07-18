// core 層の単体テスト。フレームワーク不使用の軽量 CHECK マクロで検証する。
// 実行: build/<preset>/tests/core_tests.exe (ctest からも起動される)

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <format>
#include <iostream>
#include <mutex>

#include "core/app.h"
#include "core/config.h"
#include "core/dib.h"
#include "core/edit.h"
#include "core/geometry.h"
#include "core/image_cache.h"
#include "core/image_list.h"
#include "core/keymap.h"
#include "core/pixel_convert.h"
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
    CHECK(list.at(3).filename() == L"d.png");
    CHECK(list.jumpTo(4));
    CHECK(list.index() == 4);
    CHECK(!list.jumpTo(4));  // 同じ位置は false
    CHECK(!list.jumpTo(5));  // 範囲外は無視
    CHECK(list.index() == 4);
    CHECK(list.jumpTo(2));

    const auto order = list.prefetchOrder(2);
    CHECK(order.size() == 4);
    CHECK(order[0].filename() == L"d.png");  // +1 が最優先
    CHECK(order[1].filename() == L"b.png");
    CHECK(order[2].filename() == L"e.png");
    CHECK(order[3].filename() == L"a.png");
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
    void setTitle(const std::wstring& title) override { lastTitle = title; }
    void setFullscreen(bool enabled) override { fullscreen = enabled; }
    bool isFullscreen() const override { return fullscreen; }
    std::optional<std::filesystem::path> showOpenDialog() override { return std::nullopt; }
    std::optional<std::filesystem::path> showSaveDialog(
        const std::wstring& defaultFileName) override {
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
    std::optional<std::wstring> showTextInput() override { return textInput; }
    std::optional<uint32_t> showColorPicker(uint32_t initialRGB) override {
        ++colorPickerCount;
        lastColorPickerInitial = initialRGB;
        return colorChoice;
    }
    void startTimer(unsigned milliseconds) override { lastTimerMs = milliseconds; }
    void quit() override {}

    bool fullscreen = false;
    unsigned lastTimerMs = 0;
    std::wstring lastTitle;
    std::optional<std::filesystem::path> savePath;  // 保存ダイアログの応答 (nullopt = キャンセル)
    int saveDialogCount = 0;
    std::wstring lastDefaultName;
    std::optional<size_t> menuChoice;  // メニューの応答 (nullopt = キャンセル)
    std::deque<size_t> menuQueue;      // 空でなければ menuChoice より優先
    int menuCount = 0;
    std::vector<MenuItem> lastMenuItems;
    std::optional<std::wstring> textInput;  // テキスト入力の応答 (nullopt = キャンセル)
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
        return true;
    }
    bool setText(const std::wstring& text) override {
        lastText = text;
        return true;
    }
    std::shared_ptr<DecodedImage> getImage() override { return pasteImage; }

    int imageCount = 0;
    uint32_t lastWidth = 0;
    std::wstring lastText;
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
    const std::filesystem::path path = L"C:/pics/black.png";
    fileSystem.files = {path};
    app.openPath(path);
    {
        std::unique_lock lock(mutex);
        CHECK(cv.wait_for(lock, std::chrono::seconds(5), [&] { return decoded; }));
    }
    app.onDecodeCompleted();
    CHECK(app.currentImage() != nullptr);
    CHECK(app.statusBar().leftText == L"1 x 1 px");

    // 1x1 画像はビューポート 800x(600-26) の中央 (400, 287) に等倍表示される。
    // その位置にカーソルを置くとピクセル (0,0) の座標と色が出る
    app.onMouseMove({400, 287});
    CHECK(app.statusBar().rightText == L"(0, 0)  #000000  RGB(0, 0, 0)");

    // ステータスバー上・画像外では表示しない
    app.onMouseMove({400, 590});
    CHECK(app.statusBar().rightText.empty());
    app.onMouseMove({400, 287});
    CHECK(!app.statusBar().rightText.empty());
    app.onMouseLeave();
    CHECK(app.statusBar().rightText.empty());

    // コピーで通知が出て、タイマー満了で画像情報表示に戻る
    app.execute(Command::CopyImage);
    CHECK(app.statusBar().leftText == L"画像をクリップボードにコピーしました");
    CHECK(host.lastTimerMs == 3000);
    app.execute(Command::CopyPath);
    CHECK(app.statusBar().leftText == L"パスをコピーしました: " + path.wstring());
    app.onTimer();
    CHECK(app.statusBar().leftText == L"1 x 1 px");

    // トグルとフルスクリーンで非表示になる
    app.execute(Command::ToggleStatusBar);
    CHECK(!app.statusBar().visible);
    app.execute(Command::ToggleStatusBar);
    CHECK(app.statusBar().visible);
    host.fullscreen = true;
    CHECK(!app.statusBar().visible);
    host.fullscreen = false;
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
    CHECK(app.statusBar().leftText == L"保存する画像がありません");

    // クリップボードが空のときの貼り付け
    app.execute(Command::PasteImage);
    CHECK(app.currentImage() == nullptr);
    CHECK(app.statusBar().leftText == L"クリップボードに画像がありません");

    // フォルダの画像 (FakeDecoder の 1x1) を開く
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
    CHECK(host.lastTitle.find(L"(クリップボード)") == 0);
    CHECK(app.statusBar().leftText == L"2 x 1 px");

    // デコード完了通知が来ても貼り付け画像は上書きされない
    app.onDecodeCompleted();
    CHECK(app.currentImage()->width == 2);

    // 貼り付け表示中はパスのコピーを拒否(一覧のパスとは無関係のため)
    app.execute(Command::CopyPath);
    CHECK(app.statusBar().leftText == L"コピーするパスがありません");

    // 貼り付け画像の保存: 既定名は「クリップボード.png」
    host.savePath = std::filesystem::path(L"C:/out/pasted.png");
    app.execute(Command::SaveImageAs);
    CHECK(host.lastDefaultName == L"クリップボード.png");
    CHECK(encoder.lastPath == std::filesystem::path(L"C:/out/pasted.png"));
    CHECK(encoder.lastWidth == 2);
    CHECK(app.statusBar().leftText == L"保存しました: C:/out/pasted.png");

    // 次へ移動でフォルダ一覧の表示に戻る(1枚しかなくても)
    app.execute(Command::NextImage);
    CHECK(app.currentImage() && app.currentImage()->width == 1);
    CHECK(host.lastTitle.find(L"a.png") != std::wstring::npos);

    // 通常画像の保存: 既定名は元ファイル名の .png 置き換え
    host.savePath = std::filesystem::path(L"C:/out/copy.jpg");
    app.execute(Command::SaveImageAs);
    CHECK(host.lastDefaultName == L"a.png");
    CHECK(encoder.lastWidth == 1);
    CHECK(app.statusBar().leftText == L"保存しました: C:/out/copy.jpg");

    // ダイアログのキャンセル: エンコードもメッセージも発生しない
    app.onTimer();  // 前のメッセージを消す
    host.savePath.reset();
    const int encodeCountBefore = encoder.encodeCount;
    app.execute(Command::SaveImageAs);
    CHECK(encoder.encodeCount == encodeCountBefore);
    CHECK(app.statusBar().leftText == L"1 x 1 px");

    // 保存失敗
    encoder.ok = false;
    host.savePath = std::filesystem::path(L"C:/out/x.png");
    app.execute(Command::SaveImageAs);
    CHECK(app.statusBar().leftText == L"保存に失敗しました: C:/out/x.png");
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
        fileSystem.files.push_back(std::format(L"C:/pics/f{:02}.png", i));
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
    CHECK(sb.items[0].text == L"f01.png");
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
    CHECK(host.lastTitle.find(L"f05.png") == 0);
    // ビューポート上のクリックは消費しない(パン開始に回す)
    CHECK(!app.onMouseDown({500, 300}));
    // サイドバー幅内でもステータスバーの高さでは消費のみ(ジャンプしない)
    CHECK(app.onMouseDown({100, 590}));
    CHECK(host.lastTitle.find(L"f05.png") == 0);

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
    CHECK(host.lastTitle.find(L"(クリップボード)") == 0);
    CHECK(app.onMouseDown({100, 100}));  // index 4 = 現在項目
    CHECK(host.lastTitle.find(L"f05.png") == 0);
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
    // 末端項目: 編集6種 + 太さ7 + 文字サイズ7 + 回転8 + 色1 = 29
    CHECK(countMenuLeaves(host.lastMenuItems) == 29);
    CHECK(app.currentImage()->width == 8);
    CHECK(rasterizer.rasterizeCount == 0);

    // トリミング: 画像座標 (0,0)-(4,4) → 4x4
    host.menuChoice = 0;
    app.onRightDragStart({396, 283});
    app.onRightDragEnd({400, 287});
    CHECK(app.currentImage()->width == 4 && app.currentImage()->height == 4);
    CHECK(app.statusBar().leftText == L"4 x 4 px");
    CHECK(host.lastTitle.find(L"(編集済み)") != std::wstring::npos);
    CHECK(!app.selection().visible);

    // Undo で元に戻る。履歴が空ならメッセージ
    app.execute(Command::Undo);
    CHECK(app.currentImage()->width == 8);
    CHECK(host.lastTitle.find(L"(編集済み)") == std::wstring::npos);
    app.execute(Command::Undo);
    CHECK(app.statusBar().leftText == L"取り消す編集はありません");
    app.onTimer();

    // 矩形: ラスタライズ結果 (2x2 赤) が (1,1) へ合成される
    host.menuChoice = 1;
    rasterizer.overlayWidth = 2;
    rasterizer.overlayHeight = 2;
    rasterizer.overlayX = 1;
    rasterizer.overlayY = 1;
    app.onRightDragStart({396, 283});
    app.onRightDragEnd({400, 287});
    CHECK(rasterizer.rasterizeCount == 1);
    CHECK(rasterizer.lastSpec.kind == AnnotationSpec::Kind::Rect);
    CHECK(nearly(rasterizer.lastSpec.p1.x, 0) && nearly(rasterizer.lastSpec.p2.x, 4));
    CHECK(nearly(rasterizer.lastSpec.strokeWidth, 3));  // 等倍表示なので画面基準の値のまま
    CHECK(rasterizer.lastSpec.colorRGB == 0xFF3B30);
    {
        const DecodedImage& edited = *app.currentImage();
        CHECK(edited.width == 8);
        CHECK(edited.pixels[(1 * 8 + 1) * 4 + 2] == 255);  // (1,1) は赤
        CHECK(edited.pixels[(1 * 8 + 1) * 4 + 0] == 0);
        CHECK(edited.pixels[2] == 0);  // (0,0) は青のまま
    }
    // 貼り付け元(キャッシュ相当)の画像は書き換えられていない
    CHECK(source->pixels[(1 * 8 + 1) * 4 + 2] == 0);

    // テキスト: 入力キャンセルなら何もしない。入力があれば合成される
    host.menuChoice = 5;
    host.textInput = std::nullopt;
    app.onRightDragStart({396, 283});
    app.onRightDragEnd({400, 287});
    CHECK(rasterizer.rasterizeCount == 1);
    host.textInput = L"メモ";
    app.onRightDragStart({396, 283});
    app.onRightDragEnd({400, 287});
    CHECK(rasterizer.rasterizeCount == 2);
    CHECK(rasterizer.lastSpec.kind == AnnotationSpec::Kind::Text);
    CHECK(rasterizer.lastSpec.text == L"メモ");
    CHECK(nearly(rasterizer.lastSpec.fontSize, 18));  // 既定値 (等倍表示)
    CHECK(nearly(rasterizer.lastSpec.angleDeg, 0));

    // 設定変更を選ぶとメニューが再表示され、選択領域を保ったまま続けて編集できる。
    // 末端 index: 0-5 編集, 6-12 太さ {1,2,3,5,8,12,20}, 13-19 文字サイズ
    // {12,14,18,24,36,48,72}, 20-27 回転 {0,15,30,45,90,135,180,270}, 28 色
    host.menuChoice = std::nullopt;
    host.menuQueue = {10 /*太さ8px*/, 24 /*回転90°*/, 1 /*矩形*/};
    app.onRightDragStart({396, 283});
    app.onRightDragEnd({400, 287});
    CHECK(host.menuQueue.empty());
    CHECK(rasterizer.rasterizeCount == 3);
    CHECK(nearly(rasterizer.lastSpec.strokeWidth, 8));
    CHECK(nearly(rasterizer.lastSpec.angleDeg, 90));

    // 文字サイズ 24px + 複数行テキスト。変更済みの太さ・回転も引き継がれる
    host.textInput = L"1行目\n2行目";
    host.menuQueue = {16 /*文字24px*/, 5 /*テキスト*/};
    app.onRightDragStart({396, 283});
    app.onRightDragEnd({400, 287});
    CHECK(rasterizer.rasterizeCount == 4);
    CHECK(rasterizer.lastSpec.text == L"1行目\n2行目");
    CHECK(nearly(rasterizer.lastSpec.fontSize, 24));
    CHECK(nearly(rasterizer.lastSpec.angleDeg, 90));

    // 色の変更: ダイアログの結果が以降の編集に使われる。キャンセルなら元のまま
    host.colorChoice = 0x00CC66;
    host.menuQueue = {28 /*色*/, 4 /*直線*/};
    app.onRightDragStart({396, 283});
    app.onRightDragEnd({400, 287});
    CHECK(host.colorPickerCount == 1);
    CHECK(host.lastColorPickerInitial == 0xFF3B30);
    CHECK(rasterizer.lastSpec.colorRGB == 0x00CC66);
    host.colorChoice = std::nullopt;
    host.menuQueue = {28 /*色 (キャンセル)*/, 4 /*直線*/};
    app.onRightDragStart({396, 283});
    app.onRightDragEnd({400, 287});
    CHECK(host.colorPickerCount == 2);
    CHECK(rasterizer.lastSpec.colorRGB == 0x00CC66);

    // 設定変更だけしてメニューをキャンセル → 編集は適用されず設定は残る
    const int rasterizeCountBefore = rasterizer.rasterizeCount;
    host.menuQueue = {8 /*太さ3px*/};  // 続くメニュー再表示は menuChoice = nullopt で閉じる
    app.onRightDragStart({396, 283});
    app.onRightDragEnd({400, 287});
    CHECK(rasterizer.rasterizeCount == rasterizeCountBefore);
    CHECK(!app.selection().visible);
    host.menuQueue = {1 /*矩形*/};
    app.onRightDragStart({396, 283});
    app.onRightDragEnd({400, 287});
    CHECK(nearly(rasterizer.lastSpec.strokeWidth, 3));  // 3px に戻っている

    // ラスタライズ失敗はメッセージのみ
    rasterizer.ok = false;
    host.menuChoice = 3;  // 矢印
    app.onRightDragStart({396, 283});
    app.onRightDragEnd({400, 287});
    CHECK(app.statusBar().leftText == L"描画に失敗しました");
    rasterizer.ok = true;
    app.onTimer();

    // 画像移動で編集は破棄される(一覧が空なので表示もなくなる)
    app.execute(Command::NextImage);
    CHECK(app.statusBar().leftText == L"編集を破棄しました");
    CHECK(app.currentImage() == nullptr);
    CHECK(host.lastTitle == L"Blinker");
    app.execute(Command::Undo);
    CHECK(app.statusBar().leftText == L"取り消す編集はありません");
}

} // namespace

int main() {
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
    testAppEdit();

    if (g_failures == 0) {
        std::cout << "all tests passed\n";
        return 0;
    }
    std::cout << g_failures << " check(s) failed\n";
    return 1;
}
