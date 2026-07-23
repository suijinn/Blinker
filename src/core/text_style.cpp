#include "core/text_style.h"

#include <algorithm>

namespace blinker {
namespace {

/// 位置以外の書式が同じか(隣接範囲をまとめてよいかの判定)
bool sameAttributes(const TextStyleRun& a, const TextStyleRun& b) {
    if (a.bold != b.bold || a.italic != b.italic || a.underline != b.underline ||
        a.hasColor != b.hasColor || a.fontFamily != b.fontFamily) {
        return false;
    }
    return !a.hasColor || a.colorRGB == b.colorRGB;
}

bool flagOf(const TextStyleRun& run, TextStyleFlag flag) {
    switch (flag) {
    case TextStyleFlag::Bold:
        return run.bold;
    case TextStyleFlag::Italic:
        return run.italic;
    case TextStyleFlag::Underline:
        return run.underline;
    }
    return false;
}

/// 属性を 1 つも指定していない範囲(捨ててよい範囲)か
bool isDefaultStyle(const TextStyleRun& run) {
    return !run.bold && !run.italic && !run.underline && !run.hasColor &&
           run.fontFamily.empty();
}

/**
 * [begin, end) を覆うように範囲を切り分け、覆う部分だけ fn で書き換える。
 * 範囲の外にはみ出す部分と、まだどの範囲にも属していない隙間も正しく扱う。
 */
template <typename Fn>
void applyToRange(std::vector<TextStyleRun>& runs, size_t begin, size_t end, Fn fn) {
    if (end <= begin) return;
    normalizeTextStyles(runs);  // 昇順・重なりなしを前提に 1 回の走査で切り分ける
    std::vector<TextStyleRun> out;
    out.reserve(runs.size() + 2);
    size_t pos = begin;  // [begin, end) のうち、まだ書式を置いていない先頭
    for (const TextStyleRun& r : runs) {
        if (r.end <= begin || r.begin >= end) {
            out.push_back(r);  // 対象範囲と重ならない範囲はそのまま残す
            continue;
        }
        if (r.begin < begin) {
            TextStyleRun head = r;  // 対象範囲より手前へはみ出す部分
            head.end = begin;
            out.push_back(head);
        }
        if (r.begin > pos) {
            TextStyleRun gap{pos, r.begin};  // 既存の範囲が無い隙間
            fn(gap);
            out.push_back(gap);
        }
        TextStyleRun mid = r;
        mid.begin = std::max(r.begin, begin);
        mid.end = std::min(r.end, end);
        fn(mid);
        out.push_back(mid);
        pos = mid.end;
        if (r.end > end) {
            TextStyleRun tail = r;  // 対象範囲より後ろへはみ出す部分
            tail.begin = end;
            out.push_back(tail);
        }
    }
    if (pos < end) {
        TextStyleRun gap{pos, end};
        fn(gap);
        out.push_back(gap);
    }
    runs = std::move(out);
    normalizeTextStyles(runs);
}

} // namespace

void normalizeTextStyles(std::vector<TextStyleRun>& runs) {
    std::erase_if(runs, [](const TextStyleRun& r) {
        return r.end <= r.begin || isDefaultStyle(r);
    });
    std::sort(runs.begin(), runs.end(),
              [](const TextStyleRun& a, const TextStyleRun& b) { return a.begin < b.begin; });
    std::vector<TextStyleRun> merged;
    merged.reserve(runs.size());
    for (const TextStyleRun& r : runs) {
        if (!merged.empty() && merged.back().end == r.begin &&
            sameAttributes(merged.back(), r)) {
            merged.back().end = r.end;
        } else {
            merged.push_back(r);
        }
    }
    runs = std::move(merged);
}

TextStyleRun textStyleAt(const std::vector<TextStyleRun>& runs, size_t offset) {
    for (const TextStyleRun& r : runs) {
        if (offset >= r.begin && offset < r.end) return r;
    }
    return {offset, offset};
}

bool isTextStyleFlagSet(const std::vector<TextStyleRun>& runs, size_t begin, size_t end,
                        TextStyleFlag flag) {
    if (end <= begin) return false;
    // 隙間の有無を素直に見たいので、並べ替え済みのコピーで走査する(範囲数は少ない)
    std::vector<TextStyleRun> sorted = runs;
    normalizeTextStyles(sorted);
    size_t pos = begin;
    for (const TextStyleRun& r : sorted) {
        if (r.end <= pos) continue;
        if (r.begin > pos) return false;  // 書式の無い隙間がある
        if (!flagOf(r, flag)) return false;
        pos = r.end;
        if (pos >= end) return true;
    }
    return false;
}

void setTextStyleFlag(std::vector<TextStyleRun>& runs, size_t begin, size_t end,
                      TextStyleFlag flag, bool enabled) {
    applyToRange(runs, begin, end, [flag, enabled](TextStyleRun& run) {
        switch (flag) {
        case TextStyleFlag::Bold:
            run.bold = enabled;
            break;
        case TextStyleFlag::Italic:
            run.italic = enabled;
            break;
        case TextStyleFlag::Underline:
            run.underline = enabled;
            break;
        }
    });
}

void setTextStyleColor(std::vector<TextStyleRun>& runs, size_t begin, size_t end,
                       uint32_t colorRGB) {
    applyToRange(runs, begin, end, [colorRGB](TextStyleRun& run) {
        run.hasColor = true;
        run.colorRGB = colorRGB;
    });
}

void setTextStyleFontFamily(std::vector<TextStyleRun>& runs, size_t begin, size_t end,
                            const std::string& family) {
    applyToRange(runs, begin, end, [&family](TextStyleRun& run) { run.fontFamily = family; });
}

void adjustTextStyles(std::vector<TextStyleRun>& runs, size_t offset, size_t removed,
                      size_t inserted) {
    if (removed == 0 && inserted == 0) return;
    const size_t removeEnd = offset + removed;
    // 削除で詰めてから挿入で押し出す。offset ちょうどの位置は挿入分だけ後ろへ動くので、
    // offset で終わる範囲は挿入分を取り込み、offset から始まる範囲はまるごとずれる
    const auto mapPos = [&](size_t p) {
        if (p > offset) p = p >= removeEnd ? p - removed : offset;
        return p >= offset ? p + inserted : p;
    };
    for (TextStyleRun& r : runs) {
        r.begin = mapPos(r.begin);
        r.end = mapPos(r.end);
    }
    normalizeTextStyles(runs);  // 削除で潰れて空になった範囲はここで落ちる
}

} // namespace blinker
