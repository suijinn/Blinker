#pragma once

#include <filesystem>
#include <string>

/**
 * @file image_origin.h
 * @brief 表示中の画像の出どころ ― どこから来た絵で、読めたか、元と食い違っているか。
 */

namespace blinker {

/**
 * @brief 表示中の画像の出どころと、元との関係。
 *
 * 出どころは「フォルダ一覧のファイル」「クリップボードから貼り付けた画像
 * (一覧の位置から切り離される)」「まだ何も無い」の 3 通り。
 * OS ヘッダに依存しない(単体テスト対象)。**画素も一覧も窓も知らない** ―
 * 表示中の 1 枚に付いて回る素性だけを持つ。
 *
 * 状態を変える口は setFile / setFailed / setLoading / setClipboard / clear の 5 つだけに
 * してある。読み込み失敗の記録(failed / error)を消し忘れると、次の画像に移っても
 * 古い理由がステータスバーに残り続けるため。貼り付け画像かどうかも同じで、
 * 消し忘れると一覧へ戻ったのにタイトルが「(クリップボード)」のままになる。
 *
 * @note UI スレッド専用(App と同じ)。スレッド安全ではない。
 */
class ImageOrigin {
public:
    /**
     * @brief ファイルから読めた画像として記録する。
     * @param[in] path 読み込み元のパス。
     * @note 貼り付け画像の印と読み込み失敗の記録は消える。
     */
    void setFile(std::filesystem::path path);

    /**
     * @brief ファイルの読み込みに失敗したと記録する。
     * @param[in] path  読み込もうとしたパス。
     * @param[in] error 失敗の理由(UTF-8。段階と HRESULT。ステータスバーに出す)。
     */
    void setFailed(std::filesystem::path path, std::string error);

    /**
     * @brief デコード待ちにする。
     * @note 前の画像を表示したまま待つので、パスは直前のものを残す(一覧の現在位置と
     *       食い違っていることが「読み込み中」の判定になる)。失敗の記録と
     *       貼り付け画像の印は落とす。
     */
    void setLoading();

    /**
     * @brief クリップボードから貼り付けた画像として記録する。
     * @note パスは空になる(一覧へ戻ったときに必ず読み直させるため)。
     */
    void setClipboard();

    /// @brief 表示する画像が無い状態に戻す。
    /// @note 未保存の編集の印 (edited) は変えない(画像の破棄は呼び出し側の担当)。
    void clear();

    /// @brief 表示中の画像の読み込み元を返す。
    /// @return パス。貼り付け画像・未読み込みなら空。
    const std::filesystem::path& path() const { return path_; }

    /// @brief 貼り付け画像かを返す。
    /// @return クリップボード由来なら true。
    bool fromClipboard() const { return clipboard_; }

    /// @brief 読み込みに失敗したかを返す。
    /// @return 失敗していれば true。
    bool failed() const { return failed_; }

    /// @brief 読み込み失敗の理由を返す。
    /// @return 理由(UTF-8)。失敗していない、または理由が分からなければ空。
    const std::string& error() const { return error_; }

    /// @brief 未保存の編集があるかを返す。
    /// @return 表示中の画素が出どころと食い違っていれば true。
    bool edited() const { return edited_; }

    /**
     * @brief 未保存の編集の有無を設定する。
     * @param[in] edited 編集があるなら true。
     * @return 状態が変わったら true。変わらなければ false。
     * @note 戻り値は「タイトルを更新する」「破棄した旨を通知する」といった一度きりの
     *       後始末を、呼ぶ側が毎回まで持たずに済ませるためのもの。
     */
    bool setEdited(bool edited);

private:
    std::filesystem::path path_;  ///< 読み込み元のパス。貼り付け画像・未読み込みなら空
    std::string error_;           ///< 読み込み失敗の理由(UTF-8)
    bool clipboard_ = false;      ///< クリップボードから貼り付けた画像か
    bool failed_ = false;         ///< 読み込みに失敗したか
    bool edited_ = false;         ///< 未保存の編集(トリミング・注釈)があるか
};

}  // namespace blinker
