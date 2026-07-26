#pragma once

#include <string>
#include <vector>

#include "core/edit.h"
#include "platform/decoder.h"

/**
 * @file ocr.h
 * @brief 文字認識 (OCR) のプラットフォーム抽象。
 */

namespace blinker {

/**
 * @brief OCR が認識した 1 行。
 *
 * bounds は行に含まれる単語の外接矩形。エンジンは単語単位でしか位置を返さないため、
 * 行の矩形はその和として組み立てられる(認識結果に傾きがあると実際の文字より広くなる)。
 */
struct OcrLine {
    std::string text;  ///< 行のテキスト(UTF-8)
    RectI bounds;      ///< 行の外接矩形(画像ピクセル)
};

/**
 * @brief OCR の認識結果。
 *
 * 行の順序はエンジンが返したまま(おおむね上から下)。テキストへの整形は
 * core/ocr_text.h の ocrResultToText が行う。
 */
struct OcrResult {
    std::vector<OcrLine> lines;  ///< 認識された行。1 行も無ければ空
    std::string language;        ///< 認識に使った言語タグ (BCP-47。例 "ja")
};

/**
 * @brief 文字認識 (OCR) のプラットフォーム抽象。
 *
 * Windows 実装は Windows.Media.Ocr (ocr_winrt)、SDL バックエンドは未対応の
 * スタブ (ocr_stub)。OcrService のワーカースレッドから呼ばれるため、実装は
 * スレッド安全にすること。
 */
class IOcrEngine {
public:
    virtual ~IOcrEngine() = default;

    /**
     * @brief 画像から文字を認識する。
     *
     * 認識器の準備(エンジン生成・言語の解決)は最初の呼び出しまで遅延させること。
     * 起動時間に影響させないための約束であり、実装側の裁量ではない。
     *
     * @param[in]  image  認識対象の画像(32bpp PBGRA)。透明部分は呼び出し側で
     *                    背景色へ焼き込んでおくこと。
     * @param[out] result 認識結果。成功時のみ書き込まれる。
     * @param[out] error  非 nullptr のとき、失敗した場合に限り原因を表す短い UTF-8
     *                    文字列が入る。成功時は変更しない。ステータスバーへ
     *                    そのまま表示される想定。
     * @return 認識できたら true。エンジンが使えない・失敗した場合は false。
     * @note 1 文字も認識できなかった場合は「成功して result.lines が空」になる。
     *       これは失敗ではない。
     */
    virtual bool recognize(const DecodedImage& image, OcrResult* result,
                           std::string* error = nullptr) = 0;
};

} // namespace blinker
