#include "core/view_text.h"

#include <cmath>
#include <cstdint>
#include <format>

#include "core/pixel_convert.h"

namespace blinker {

namespace {

// 表示用のズーム率(タイトルに出す整数のパーセント)
int zoomPercent(const float zoom) {
    return static_cast<int>(std::lround(zoom * 100));
}

} // namespace

std::string windowTitle(const TitleState& state) {
    if (state.fromClipboard) {
        return std::format("(クリップボード){} {}% - {}", state.edited ? " (編集中)" : "",
                           zoomPercent(state.zoom), state.appName);
    }
    if (state.count == 0) return std::string(state.appName);
    std::string title =
        std::format("{} [{}/{}]", state.filename, state.index + 1, state.count);
    if (state.failed) {
        title += " (読み込み失敗)";
    } else if (state.loading) {
        title += " (読み込み中)";
    } else {
        if (state.edited) title += " (編集中)";
        title += std::format(" {}%", zoomPercent(state.zoom));
    }
    title += std::format(" - {}", state.appName);
    return title;
}

std::string statusText(const StatusTextState& state) {
    if (!state.message.empty()) return std::string(state.message);
    if (state.image) {
        const DecodedImage& image = *state.image;
        // 編集ドラッグが何をするかは見た目に出ないので、現在のツールをここに出す。
        // 縮小して取り込んだ画像は、ズーム率が元の大きさに対する比でなくなるので明示する
        std::string text = image.downscaled()
                               ? std::format("{} x {} px (元 {} x {} を縮小表示)", image.width,
                                             image.height, image.sourceWidth, image.sourceHeight)
                               : std::format("{} x {} px", image.width, image.height);
        // 何枚目を見ているのかはタイトルの [i/n](フォルダ内の位置)では分からない
        if (state.frame) {
            const FrameStatus& frame = *state.frame;
            text += std::format("  |  {} {}/{}", frame.unitLabel, frame.index + 1, frame.count);
            if (!frame.label.empty()) text += std::format(" ({})", frame.label);
            if (frame.animation) text += frame.playing ? " 再生中" : " 停止中";
        }
        // なぜ隣のフォルダの画像が出てくるのか分からなくなるので、再帰中は常に見せる
        if (state.recursive) text += "  |  サブフォルダ含む";
        text += std::format("  |  ツール: {}", toolLabel(state.tool));
        // 画像を送れない理由が画面から分かるようにする(「モードが見えないと誤操作になる」)。
        // ロックが切ってあれば出さない ― 出しても操作は何も変わらないため
        if (state.editLocked) text += "  |  編集中(遷移ロック)";
        return text;
    }
    if (state.failed) {
        // 失敗した段階とコードまで出す(現物が手元にない不具合を切り分けられるように)
        return state.error.empty() ? "読み込み失敗"
                                   : std::format("読み込み失敗: {}", state.error);
    }
    return {};
}

std::string objectSizeText(const int width, const int height) {
    if (width <= 0 || height <= 0) return {};
    return std::format("選択 {} x {}", width, height);
}

std::string pixelInfoText(const DecodedImage& image, const int x, const int y) {
    if (x < 0 || y < 0 || x >= static_cast<int>(image.width) ||
        y >= static_cast<int>(image.height)) {
        return {};
    }
    const uint8_t* px = image.pixels.data() + (static_cast<size_t>(y) * image.width + x) * 4;
    const uint8_t a = px[3];
    const uint8_t r = unpremultiply(px[2], a);
    const uint8_t g = unpremultiply(px[1], a);
    const uint8_t b = unpremultiply(px[0], a);
    std::string text = std::format("({}, {})  #{:02X}{:02X}{:02X}", x, y, r, g, b);
    text += a == 255 ? std::format("  RGB({}, {}, {})", r, g, b)
                     : std::format("  RGBA({}, {}, {}, {})", r, g, b, a);
    return text;
}

} // namespace blinker
