#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "stb/stb_truetype.h"

namespace blinker {

// stb_truetype による UI テキスト描画(SDL バックエンド用)。
// システムフォント(日本語対応を優先)を候補パスから読み込み、コードポイント単位の
// グリフキャッシュを持つ。BGRA(事前乗算)バッファへの CPU 合成のみを行い、
// テクスチャ化は呼び出し側(RendererSdl)の責務。UI スレッドからのみ呼ばれる。
class FontStb {
public:
    // configPath(blinker.ini の [view] font_path)を最優先に、既定候補の順で読み込む。
    // 全滅なら false(テキストなしで動作は継続できる)
    bool load(const std::string& configPath);
    bool ok() const { return loaded_; }
    const std::string& loadedPath() const { return loadedPath_; }

    // 高さ heightPx のときのベースライン位置(上端からの距離)と行の高さ
    float ascent(float heightPx) const;
    float lineHeight(float heightPx) const;

    // 1 行分のテキスト幅(px)。改行以降は無視する
    float measure(std::string_view utf8, float heightPx);

    // dst (BGRA 事前乗算、stride バイト) の (x, top) を左上として 1 行描画する。
    // クリップはバッファ境界で行う。改行以降は無視する
    void drawText(uint8_t* dst, int dstWidth, int dstHeight, int strideBytes, float x,
                  float top, std::string_view utf8, float heightPx, uint32_t rgb);

private:
    struct Glyph {
        int width = 0, height = 0;
        int xoff = 0, yoff = 0;      // 描画原点(ベースライン左端)からのオフセット
        float advance = 0;
        std::vector<uint8_t> bitmap;  // 8bit カバレッジ (width * height)
    };
    const Glyph& glyphFor(char32_t cp, float heightPx);
    bool loadFile(const std::string& path);

    bool loaded_ = false;
    std::string loadedPath_;
    std::vector<uint8_t> fontData_;
    stbtt_fontinfo info_{};
    int rawAscent_ = 0, rawDescent_ = 0, rawLineGap_ = 0;
    std::unordered_map<uint64_t, Glyph> cache_;  // key = (高さ(1/4px単位) << 32) | cp
};

} // namespace blinker
