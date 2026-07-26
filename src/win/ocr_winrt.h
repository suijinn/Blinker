#pragma once

#include <mutex>
#include <string>

#include "platform/ocr.h"

/**
 * @file ocr_winrt.h
 * @brief Windows.Media.Ocr による文字認識(Windows 実装)。
 */

namespace blinker {

/**
 * @brief Windows.Media.Ocr による文字認識。
 *
 * WinRT を ABI で直接呼ぶ。combase.dll は実行時に LoadLibrary で解決するため、
 * exe の静的インポートは 1 つも増えず、OCR を使わない限り読み込まれもしない
 * (起動時間と、WinRT の無い環境での起動可否に影響させないため)。
 *
 * 認識器の生成は最初の recognize まで遅延する。生成に失敗する主な原因は
 * 言語パック(OCR 認識器)が入っていないことで、その旨を error に入れて返す。
 *
 * 呼び出しは内部で排他され、スレッドごとに WinRT (MTA) を初期化する。ただし
 * 既に STA になっているスレッド(UI スレッドなど)からは使えない。非同期完了を
 * ブロックして待つため STA ではデッドロックするので、その場合は失敗を返す。
 * OcrService のワーカースレッドから呼ぶこと。
 */
class OcrEngineWinrt final : public IOcrEngine {
public:
    /**
     * @brief 使用する言語を指定して構築する。
     * @param[in] languageTag 認識に使う言語タグ (BCP-47。例 "ja" "en-US")。
     *                        空ならユーザーの表示言語から自動で選ぶ。
     * @note ここでは何も初期化しない(最初の recognize まで遅延する)。
     */
    explicit OcrEngineWinrt(std::string languageTag = {});

    /// @brief 保持している WinRT オブジェクトを解放する。
    ~OcrEngineWinrt() override;

    OcrEngineWinrt(const OcrEngineWinrt&) = delete;
    OcrEngineWinrt& operator=(const OcrEngineWinrt&) = delete;

    /**
     * @brief 画像から文字を認識する。
     *
     * 文字が小さいと精度が大きく落ちるため、1 回目の認識で得た行の高さが閾値未満なら
     * 拡大して読み直す(2 パス)。どれだけ拡大すべきかは認識するまで分からないので、
     * 先に測ることはできない。大きい文字では拡大しても改善しないため 1 回で終わる。
     *
     * @param[in]  image  認識対象の画像(32bpp PBGRA)。
     * @param[out] result 認識結果。成功時のみ書き込まれる。
     * @param[out] error  非 nullptr のとき、失敗時に理由が入る(UTF-8)。
     * @return 認識できたら true。失敗時は false。
     * @note 認識器の上限を超える大きさの画像は縮小して認識し、座標は元の
     *       画像のスケールへ戻して返す。読み直しでも上限は超えない。
     */
    bool recognize(const DecodedImage& image, OcrResult* result,
                   std::string* error = nullptr) override;

private:
    /// @brief 認識器を(まだなら)生成する。
    /// @param[out] error 失敗理由の格納先(nullptr 可)。
    /// @return 認識器が使えるなら true。
    /// @pre mutex_ をロック済みであること。
    bool ensureEngineLocked(std::string* error);

    /**
     * @brief 与えられたビットマップを 1 回だけ認識する。
     * @param[in]  bitmapSource 認識に渡す画像(32bpp PBGRA。上限内であること)。
     * @param[in]  coordScale   認識座標を元画像スケールへ戻す倍率。
     * @param[out] result       認識結果。成功時のみ書き込まれる(language は入らない)。
     * @param[out] error        非 nullptr のとき、失敗時に理由が入る(UTF-8)。
     * @return 認識できたら true。失敗時は false。
     * @pre mutex_ をロック済みで、認識器が生成済みであること。
     */
    bool recognizeOnceLocked(const DecodedImage& bitmapSource, double coordScale,
                             OcrResult* result, std::string* error);

    std::mutex mutex_;
    std::string languageTag_;
    void* engine_ = nullptr;        ///< ABI::Windows::Media::Ocr::IOcrEngine*
    uint32_t maxDimension_ = 0;     ///< 認識器が受け付ける最大の辺の長さ(px)
    std::string engineLanguage_;    ///< 実際に選ばれた言語タグ
    bool initialized_ = false;      ///< 生成を試みたか(失敗も含む)
    std::string initError_;         ///< 生成に失敗した理由
};

} // namespace blinker
