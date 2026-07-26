#include "core/ocr_text.h"

#include <algorithm>

#include "core/str_util.h"
#include "core/unicode.h"

namespace blinker {
namespace {

// 空白を挟んでも語が続いているとみなす文字(和文・中文・韓文とその句読点・全角形)。
// 「あ い」→「あい」と詰めるのはこの範囲だけで、欧文の語間は触らない
bool isCjk(char32_t cp) {
    return (cp >= 0x3000 && cp <= 0x303F) ||    // CJK の記号・句読点
           (cp >= 0x3040 && cp <= 0x30FF) ||    // ひらがな・カタカナ
           (cp >= 0x3400 && cp <= 0x4DBF) ||    // CJK 統合漢字拡張 A
           (cp >= 0x4E00 && cp <= 0x9FFF) ||    // CJK 統合漢字
           (cp >= 0xAC00 && cp <= 0xD7A3) ||    // ハングル音節
           (cp >= 0xF900 && cp <= 0xFAFF) ||    // CJK 互換漢字
           (cp >= 0xFF00 && cp <= 0xFF60) ||    // 全角形
           (cp >= 0xFFE0 && cp <= 0xFFE6) ||    // 全角の記号
           (cp >= 0x20000 && cp <= 0x3FFFF);    // CJK 統合漢字拡張 B 以降
}

bool isSpace(char32_t cp) {
    return cp == U' ' || cp == U'\t';
}

// 読み直しの判断に使う行の高さ(px)。実測の根拠は ocrRetryUpscale の説明を参照
constexpr int kMinLineHeightPx = 20;     // これ以上あれば拡大しても改善しない
constexpr int kTargetLineHeightPx = 30;  // 読み直しで狙う行の高さ
constexpr double kMinUpscaleFactor = 1.3;  // これ未満の拡大は 1 回分の時間に見合わない
constexpr double kMaxUpscaleFactor = 4.0;  // 拡大しすぎは精度が落ち、メモリも食う

} // namespace

std::string removeSpacesBetweenCjk(std::string_view s) {
    const std::u32string cps = utf8ToUtf32(s);
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < cps.size()) {
        if (!isSpace(cps[i])) {
            appendUtf8(out, cps[i]);
            ++i;
            continue;
        }
        // 空白の連なりをまとめて見て、前後がどちらも CJK なら丸ごと捨てる
        size_t end = i;
        while (end < cps.size() && isSpace(cps[end])) ++end;
        const bool cjkBefore = i > 0 && isCjk(cps[i - 1]);
        const bool cjkAfter = end < cps.size() && isCjk(cps[end]);
        if (!(cjkBefore && cjkAfter)) {
            for (size_t k = i; k < end; ++k) appendUtf8(out, cps[k]);
        }
        i = end;
    }
    return out;
}

std::string ocrResultToText(const OcrResult& result) {
    std::string joined;
    for (const OcrLine& line : result.lines) {
        const std::string_view text = trim(line.text);
        if (text.empty()) continue;  // 空白だけの行は連結しても情報が無い
        if (!joined.empty()) joined += '\n';
        joined += text;
    }
    return removeSpacesBetweenCjk(joined);
}

double ocrRetryUpscale(const int lineHeightPx, const double maxFactor) {
    if (lineHeightPx <= 0) return 1.0;                 // 行が無い(読み直しても同じ)
    if (lineHeightPx >= kMinLineHeightPx) return 1.0;  // 既に十分大きい
    const double factor = std::min({static_cast<double>(kTargetLineHeightPx) / lineHeightPx,
                                    maxFactor, kMaxUpscaleFactor});
    return factor < kMinUpscaleFactor ? 1.0 : factor;
}

} // namespace blinker
