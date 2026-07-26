#pragma once

#include "platform/ocr.h"

/**
 * @file ocr_stub.h
 * @brief 文字認識のスタブ(SDL バックエンド用)。
 */

namespace blinker {

/**
 * @brief 常に失敗を返す IOcrEngine 実装。
 *
 * SDL バックエンドは文字認識を未サポート。Windows 実装は OS 内蔵の
 * Windows.Media.Ocr を使っており、同等のものを外部ライブラリ依存ゼロで
 * 用意できないため(Tesseract は学習データが数十 MB になる)。
 * App 側は error をそのままステータスバーへ出して安全に継続する。
 *
 * @todo SDL バックエンドで文字認識に対応する。Linux なら tesseract を
 *       動的リンクで任意依存にする案があるが、単一バイナリ配布の方針と
 *       どう折り合いを付けるかが未決。
 */
class OcrStub final : public IOcrEngine {
public:
    /**
     * @brief 文字認識(常に失敗)。
     * @param[in]  image  未使用。
     * @param[out] result 未使用(書き込まない)。
     * @param[out] error  非 nullptr なら未対応である旨が入る。
     * @return 常に false。
     */
    bool recognize(const DecodedImage& image, OcrResult* result,
                   std::string* error = nullptr) override {
        (void)image;
        (void)result;
        if (error) *error = "このビルドでは文字認識に対応していません";
        return false;
    }
};

} // namespace blinker
