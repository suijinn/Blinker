#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "platform/file_system.h"

/**
 * @file sort_order.h
 * @brief 一覧の並び順(キーと向き)と、その適用(純関数)。
 *
 * 列挙とプラットフォーム依存の名前順比較は IFileSystem の責務で、
 * 「どのキーでどちら向きに並べるか」だけがここにある(単体テスト対象)。
 */

namespace blinker {

/**
 * @brief 一覧の並び替えに使うキー。
 */
enum class SortKey {
    Name,       ///< ファイル名(再帰時は「親フォルダ → ファイル名」の 2 段)
    Date,       ///< 最終更新時刻
    Size,       ///< ファイルサイズ
    Extension,  ///< 拡張子(大文字小文字は無視)
};

/**
 * @brief 一覧の並び順。
 */
struct SortOrder {
    SortKey key = SortKey::Name;  ///< 並び替えのキー
    bool descending = false;      ///< 降順にするか
};

/**
 * @brief 並び順を適用した表示順を求める。
 *
 * 主キーが同値の要素の順序は **常に名前昇順のまま**で、降順にしても反転しない
 * (エクスプローラの列ヘッダと同じ挙動)。そのため降順は std::reverse ではなく
 * 比較子を反転した安定ソートで実装している。
 *
 * @param[in] entries 並べ替える一覧。
 * @param[in] order   適用する並び順。
 * @return entries への添字を表示順に並べたもの(要素数は entries と同じ)。
 * @pre entries は IFileSystem::listImages の契約どおり名前昇順に並んでいること
 *      (この前提が崩れると同値の順序が不定になる)。
 */
std::vector<size_t> sortedOrder(const std::vector<FileEntry>& entries, SortOrder order);

/**
 * @brief 並び替えキーの blinker.ini 表記を返す。
 * @param[in] key 対象のキー。
 * @return "name" / "date" / "size" / "ext" のいずれか。
 */
std::string_view sortKeyIniName(SortKey key);

/**
 * @brief blinker.ini 表記から並び替えキーを求める。
 * @param[in] name 表記(前後の空白と大文字小文字は無視する)。
 * @return 対応するキー。未知の表記なら std::nullopt。
 */
std::optional<SortKey> sortKeyFromIniName(std::string_view name);

/**
 * @brief 並び順の表示名を組み立てる(ステータスバーの通知用)。
 * @param[in] order 対象の並び順。
 * @return 「更新日時 (新しい順)」のような文字列(UTF-8)。
 */
std::string sortOrderLabel(SortOrder order);

/**
 * @brief 次の並び替えキーを返す(Command::CycleSortKey 用)。
 * @param[in] key 現在のキー。
 * @return 名前 → 更新日時 → サイズ → 種類 → 名前 と巡回した次のキー。
 */
SortKey nextSortKey(SortKey key);

} // namespace blinker
