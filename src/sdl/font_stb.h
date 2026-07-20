#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "stb/stb_truetype.h"

/**
 * @file font_stb.h
 * @brief stb_truetype による UI テキスト描画(SDL バックエンド用)。
 */

namespace blinker {

/**
 * @brief stb_truetype による UI テキスト描画。
 *
 * システムフォント(日本語対応を優先)を候補パスから読み込み、コードポイント単位の
 * グリフキャッシュを持つ。BGRA(事前乗算)バッファへの CPU 合成のみを行い、
 * テクスチャ化は呼び出し側(RendererSdl)の責務。UI スレッドからのみ呼ばれる。
 */
class FontStb {
public:
    /**
     * @brief フォントを読み込む。
     * @param[in] configPath blinker.ini の [view] font_path。最優先で試す。空なら既定候補のみ。
     * @return いずれかのフォントを読み込めたら true。全滅なら false
     *         (テキストなしで動作は継続できる)。
     */
    bool load(const std::string& configPath);

    /**
     * @brief フォントを読み込み済みかを返す。
     * @return 読み込み済みなら true。
     */
    bool ok() const { return loaded_; }

    /**
     * @brief 実際に読み込んだフォントのパスを返す。
     * @return フォントファイルのパス。未読み込みなら空文字列。
     */
    const std::string& loadedPath() const { return loadedPath_; }

    /**
     * @brief 指定の文字高さにおけるベースライン位置を返す。
     * @param[in] heightPx 文字の高さ(px)。
     * @return 上端からベースラインまでの距離(px)。
     */
    float ascent(float heightPx) const;

    /**
     * @brief 指定の文字高さにおける行の高さを返す。
     * @param[in] heightPx 文字の高さ(px)。
     * @return 行送りを含む高さ(px)。
     */
    float lineHeight(float heightPx) const;

    /**
     * @brief 1 行分のテキスト幅を測る。
     * @param[in] utf8     測定する文字列(UTF-8)。改行以降は無視する。
     * @param[in] heightPx 文字の高さ(px)。
     * @return テキストの幅(px)。
     */
    float measure(std::string_view utf8, float heightPx);

    /**
     * @brief バッファへ 1 行分のテキストを描画する。
     * @param[in,out] dst         描画先バッファ(BGRA 事前乗算)。破壊的に更新する。
     * @param[in]     dstWidth    描画先の幅(ピクセル)。
     * @param[in]     dstHeight   描画先の高さ(ピクセル)。
     * @param[in]     strideBytes 描画先の 1 行のバイト数。
     * @param[in]     x           描画開始位置の左端 X 座標(px)。
     * @param[in]     top         描画開始位置の上端 Y 座標(px)。
     * @param[in]     utf8        描画する文字列(UTF-8)。改行以降は無視する。
     * @param[in]     heightPx    文字の高さ(px)。
     * @param[in]     rgb         文字色(0xRRGGBB)。
     * @note クリップはバッファ境界で行う。
     */
    void drawText(uint8_t* dst, int dstWidth, int dstHeight, int strideBytes, float x,
                  float top, std::string_view utf8, float heightPx, uint32_t rgb);

private:
    /// @brief キャッシュされた 1 グリフ分のビットマップとメトリクス。
    struct Glyph {
        int width = 0;                ///< ビットマップの幅(ピクセル)
        int height = 0;               ///< ビットマップの高さ(ピクセル)
        int xoff = 0;                 ///< 描画原点(ベースライン左端)からの X オフセット
        int yoff = 0;                 ///< 描画原点(ベースライン左端)からの Y オフセット
        float advance = 0;            ///< 次の文字までの送り幅(px)
        std::vector<uint8_t> bitmap;  ///< 8bit カバレッジ (width * height)
    };

    /**
     * @brief コードポイントのグリフをキャッシュから取得する(なければラスタライズ)。
     * @param[in] cp       取得するコードポイント。
     * @param[in] heightPx 文字の高さ(px)。
     * @return グリフへの参照。キャッシュが保持する。
     */
    const Glyph& glyphFor(char32_t cp, float heightPx);

    /**
     * @brief フォントファイルを読み込んで初期化する。
     * @param[in] path 読み込むフォントファイルのパス。
     * @return 読み込めたら true。存在しない・解析失敗なら false。
     */
    bool loadFile(const std::string& path);

    bool loaded_ = false;
    std::string loadedPath_;
    std::vector<uint8_t> fontData_;
    stbtt_fontinfo info_{};
    int rawAscent_ = 0, rawDescent_ = 0, rawLineGap_ = 0;
    std::unordered_map<uint64_t, Glyph> cache_;  ///< key = (高さ(1/4px単位) << 32) | cp
};

} // namespace blinker
