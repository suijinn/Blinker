#pragma once

#include "platform/printer.h"

/**
 * @file printer_stub.h
 * @brief 印刷のスタブ(SDL バックエンド用)。
 */

namespace blinker {

/**
 * @brief 常に未対応を返す IPrinter 実装。
 *
 * SDL バックエンドは印刷を未サポート。Windows 実装は OS の印刷ダイアログと GDI を
 * 使っており、同等のものを外部ライブラリ依存ゼロで用意できないため
 * (Linux では CUPS、macOS では Cocoa の印刷 API が要る)。
 * App 側は PrintStatus::Unsupported をステータスバーの通知に変えて安全に継続する。
 *
 * @todo SDL バックエンドで印刷に対応する。
 */
class PrinterStub final : public IPrinter {
public:
    /**
     * @brief 印刷(常に未対応)。
     * @param[in] image   未使用。
     * @param[in] jobName 未使用。
     * @param[in] options 未使用。
     * @return 常に PrintStatus::Unsupported。
     */
    PrintStatus print(const DecodedImage& image, const std::string& jobName,
                      const PrintOptions& options) override {
        (void)image;
        (void)jobName;
        (void)options;
        return PrintStatus::Unsupported;
    }
};

} // namespace blinker
