#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

/**
 * @file file_system.h
 * @brief ファイル列挙のプラットフォーム抽象。
 */

namespace blinker {

/**
 * @brief 列挙された画像ファイル 1 件分の情報。
 *
 * 並び替えのキー(core/sort_order.h)に必要なものだけを持つ。
 */
struct FileEntry {
    std::filesystem::path path;      ///< フルパス
    std::filesystem::path relative;  ///< 列挙の起点フォルダからの相対パス(表示・比較用)
    /**
     * 最終更新時刻。**大小比較にのみ使う値**で、エポックと単位は実装依存
     * (絶対時刻としての意味は持たない)。
     *
     * `std::filesystem::file_time_type` から Unix 時刻への変換は処理系差が出るが、
     * 一覧の並び替えに要るのは順序だけなので `time_since_epoch().count()` を
     * そのまま入れている。日時を表示したくなったら別途 platform 側に整形を頼むこと。
     * 取得できなかった場合は 0。
     */
    int64_t lastWriteTick = 0;
    uint64_t sizeBytes = 0;  ///< ファイルサイズ(バイト)。取得できなければ 0
};

/**
 * @brief 列挙のオプション。
 */
struct ListOptions {
    bool recursive = false;    ///< サブフォルダを再帰的に含める
    size_t maxFiles = 100000;  ///< 打ち切り上限(0 なら無制限)
};

/**
 * @brief 列挙の結果。
 */
struct ListResult {
    std::vector<FileEntry> entries;  ///< 列挙された画像(下記の順序で並ぶ)
    bool truncated = false;          ///< ListOptions::maxFiles に達して打ち切ったか
};

/**
 * @brief ファイル列挙のプラットフォーム抽象。
 *
 * Windows 実装は file_system_win、SDL バックエンドは file_system_posix。
 */
class IFileSystem {
public:
    virtual ~IFileSystem() = default;

    /**
     * @brief フォルダ内の画像ファイルを列挙する。
     *
     * 戻り値は必ず **「親フォルダの相対パス → ファイル名」の 2 段の自然順**
     * (エクスプローラ相当)で昇順に並べること。core 側の並べ替え
     * (blinker::sortedOrder)は「入力が名前昇順である」ことを前提に
     * std::stable_sort を掛けるだけで同値の順序を決めており、この契約が土台になる。
     * 相対パスをまるごと 1 本の文字列として比較しないこと(区切り文字が
     * 英数字より後ろに来るため `a/b.jpg` と `a.jpg` の並びが直感に反する)。
     *
     * 再帰時はシンボリックリンクと隠しフォルダを辿らないこと(前者は無限走査、
     * 後者は .git のような大量のファイルを踏むのを避けるため)。
     *
     * @param[in] dir     列挙の起点フォルダ。
     * @param[in] options 再帰の有無と件数上限。
     * @return 列挙結果。フォルダが読めない場合は空。
     * @note ScanService のワーカースレッドから呼ばれるため、実装はスレッド安全にすること。
     */
    virtual ListResult listImages(const std::filesystem::path& dir,
                                  const ListOptions& options) = 0;
};

} // namespace blinker
