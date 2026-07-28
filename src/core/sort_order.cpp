#include "core/sort_order.h"

#include <algorithm>
#include <array>
#include <numeric>

#include "core/str_util.h"
#include "core/unicode.h"

namespace blinker {
namespace {

struct KeyName {
    SortKey key;
    std::string_view name;
};

constexpr std::array kSortKeyNames = {
    KeyName{SortKey::Name, "name"},
    KeyName{SortKey::Date, "date"},
    KeyName{SortKey::Size, "size"},
    KeyName{SortKey::Extension, "ext"},
};

// 拡張子は比較のたびに UTF-8 化・小文字化すると重いので、ソート前に 1 回だけ作る
std::vector<std::string> extensionKeys(const std::vector<FileEntry>& entries) {
    std::vector<std::string> keys;
    keys.reserve(entries.size());
    for (const FileEntry& e : entries) keys.push_back(toLower(pathToUtf8(e.path.extension())));
    return keys;
}

} // namespace

std::vector<size_t> sortedOrder(const std::vector<FileEntry>& entries, const SortOrder order) {
    std::vector<size_t> index(entries.size());
    std::iota(index.begin(), index.end(), size_t{0});

    // 入力が名前昇順なので、名前キーは並べ替え不要。降順は同値が無いため反転でよい
    if (order.key == SortKey::Name) {
        if (order.descending) std::reverse(index.begin(), index.end());
        return index;
    }

    // 主キーだけを比較する安定ソートにすることで、同値の並びは入力(= 名前昇順)のまま残る。
    // 降順でも std::reverse は使わない(同値内の名前順まで裏返ってしまう)
    const auto stableBy = [&index, descending = order.descending](auto less) {
        std::stable_sort(index.begin(), index.end(),
                         [&less, descending](const size_t a, const size_t b) {
                             return descending ? less(b, a) : less(a, b);
                         });
    };

    switch (order.key) {
    case SortKey::Date:
        stableBy([&entries](const size_t a, const size_t b) {
            return entries[a].lastWriteTick < entries[b].lastWriteTick;
        });
        break;
    case SortKey::Size:
        stableBy([&entries](const size_t a, const size_t b) {
            return entries[a].sizeBytes < entries[b].sizeBytes;
        });
        break;
    case SortKey::Extension: {
        const std::vector<std::string> keys = extensionKeys(entries);
        stableBy([&keys](const size_t a, const size_t b) {
            return naturalCompare(keys[a], keys[b]) < 0;
        });
        break;
    }
    case SortKey::Name:
        break;  // 上で返している
    }
    return index;
}

std::string_view sortKeyIniName(const SortKey key) {
    for (const auto& e : kSortKeyNames) {
        if (e.key == key) return e.name;
    }
    return "name";
}

std::optional<SortKey> sortKeyFromIniName(const std::string_view name) {
    const std::string lower = toLower(trim(name));
    for (const auto& e : kSortKeyNames) {
        if (e.name == lower) return e.key;
    }
    return std::nullopt;
}

std::string sortOrderLabel(const SortOrder order) {
    // 向きの表現はキーによって分かりやすい語が違う(日時なら「新しい順」)
    switch (order.key) {
    case SortKey::Name:
        return order.descending ? "名前 (降順)" : "名前 (昇順)";
    case SortKey::Date:
        return order.descending ? "更新日時 (新しい順)" : "更新日時 (古い順)";
    case SortKey::Size:
        return order.descending ? "サイズ (大きい順)" : "サイズ (小さい順)";
    case SortKey::Extension:
        return order.descending ? "種類 (降順)" : "種類 (昇順)";
    }
    return {};
}

SortKey nextSortKey(const SortKey key) {
    switch (key) {
    case SortKey::Name:
        return SortKey::Date;
    case SortKey::Date:
        return SortKey::Size;
    case SortKey::Size:
        return SortKey::Extension;
    case SortKey::Extension:
        break;
    }
    return SortKey::Name;
}

} // namespace blinker
