#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

#include "platform/annotation.h"
#include "platform/decoder.h"

/**
 * @file edit_history.h
 * @brief 編集(トリミング・注釈)の取り消し・やり直し履歴。
 *
 * OS ヘッダに依存しない(単体テスト対象)。App から切り出した状態で、
 * 画像そのものや表示状態は知らない ― 「スナップショットを積む・取り出す」だけを行う。
 */

namespace blinker {

/**
 * @brief undo 1 段分のスナップショット(画像と注釈一覧)。
 *
 * 画像は共有する(トリミングのように差し替えでしか変わらないため、
 * 段ごとに画素を複製する必要はない)。
 */
struct EditSnapshot {
    std::shared_ptr<DecodedImage> image;      ///< そのときの画像
    std::vector<AnnotationSpec> annotations;  ///< そのときの注釈一覧
};

/**
 * @brief 取り消し・やり直しの履歴と、「最初の 1 回だけ積む」ための一時状態。
 *
 * ドラッグ(移動・回転・リサイズ)とテキスト入力は 1 回の操作で変更が何度も届くので、
 * 履歴へ積むのは最初の 1 回だけにする必要がある。その判定に使う旗を持つのは、
 * 積み先である履歴と旗が食い違わないようにするため。
 *
 * @note UI スレッド専用(App と同じ)。スレッド安全ではない。
 */
class EditHistory {
public:
    /// 取り消せる段数の上限。超えた分は古いほうから捨てる
    static constexpr size_t kLimit = 10;

    /**
     * @brief スナップショットを取り消し履歴へ積む(やり直し履歴は捨てる)。
     *
     * 新しい編集をした時点で、やり直せる先(分岐した未来)は無くなる。
     *
     * @param[in] state 積むスナップショット。所有権を受け取る。
     */
    void push(EditSnapshot state);

    /**
     * @brief 1 つ取り消す(取り消す前の状態はやり直し履歴へ積む)。
     * @param[in] current 現在の状態。取り消せるときだけ使われる。
     * @return 戻り先のスナップショット。取り消せるものが無ければ std::nullopt。
     */
    std::optional<EditSnapshot> undo(EditSnapshot current);

    /**
     * @brief 1 つやり直す(やり直す前の状態は取り消し履歴へ積む)。
     * @param[in] current 現在の状態。やり直せるときだけ使われる。
     * @return やり直した先のスナップショット。無ければ std::nullopt。
     */
    std::optional<EditSnapshot> redo(EditSnapshot current);

    /// @brief 履歴と一時状態をすべて捨てる(画像の切り替え・編集の破棄)。
    void clear();

    /// @brief 取り消せる編集があるか。
    /// @return 1 段以上積まれていれば true。
    bool canUndo() const { return !undo_.empty(); }

    /// @brief やり直せる編集があるか。
    /// @return 1 段以上積まれていれば true。
    bool canRedo() const { return !redo_.empty(); }

    /// @brief 履歴が両方とも空か。
    /// @return 取り消しもやり直しもできなければ true。
    bool empty() const { return undo_.empty() && redo_.empty(); }

    /// @brief ドラッグの開始・終了で呼び、「まだ積んでいない」状態へ戻す。
    void resetDrag();

    /**
     * @brief ドラッグ中の最初の変更かを判定し、旗を立てる。
     *
     * ドラッグ中は変更のたびに呼ばれる。true を返すのは resetDrag のあと 1 回だけで、
     * 呼び出し側はそのときだけ push すればよい(スナップショットを作る手間も省ける)。
     *
     * @return 最初の変更なら true。2 回目以降は false。
     */
    bool consumeDragPush();

    /// @brief ドラッグ中に履歴へ積んだか(= 実際に変更があったか)。
    /// @return consumeDragPush が true を返したあとなら true。
    bool dragPushed() const { return dragPushed_; }

    /// @brief キーでの移動の連なりを区切り、「まだ積んでいない」状態へ戻す。
    /// @note 別のコマンドを実行したときとオブジェクトを掴んだときに呼ぶ。
    void resetKeyMove();

    /**
     * @brief キーでの移動の連なりの最初の 1 回かを判定し、旗を立てる。
     *
     * ドラッグの consumeDragPush と同じ「最初の 1 回だけ積む」仕組み。矢印キーを
     * 押した回数だけ履歴が積まれると、上限 kLimit(10 段)を数回のキー入力で
     * 使い切ってしまうため、区切りまでの連続した移動をまとめて 1 段にする。
     *
     * @return 連なりの最初の移動なら true。2 回目以降は false。
     */
    bool consumeKeyMovePush();

    /**
     * @brief テキスト編集の開始時に、編集前のスナップショットを控える。
     *
     * 最初の変更が届くまで積まない(何も入力せずに終わる場合があるため)。
     *
     * @param[in] before 編集前のスナップショット。所有権を受け取る。
     */
    void beginTextEdit(EditSnapshot before);

    /**
     * @brief 控えてあるスナップショットを、最初の変更のときだけ取り出す。
     * @return 最初の変更なら beginTextEdit で控えたもの。2 回目以降は std::nullopt。
     */
    std::optional<EditSnapshot> consumeTextEditSnapshot();

    /// @brief テキスト編集中に履歴へ積んだか(= 編集前へ戻せるか)。
    /// @return consumeTextEditSnapshot が中身を返したあとなら true。
    bool textEditPushed() const { return textEditPushed_; }

private:
    /// 末尾へ積み、kLimit を超えた分を古いほうから捨てる
    static void pushTo(std::vector<EditSnapshot>& stack, EditSnapshot state);

    /// 一方から取り出し、現在の状態をもう一方へ積む(undo/redo の共通処理)
    static std::optional<EditSnapshot> transfer(std::vector<EditSnapshot>& from,
                                                std::vector<EditSnapshot>& to,
                                                EditSnapshot current);

    std::vector<EditSnapshot> undo_;  ///< 取り消し履歴(末尾が直前の状態)
    std::vector<EditSnapshot> redo_;  ///< やり直し履歴。push で捨てられる
    bool dragPushed_ = false;         ///< ドラッグ中の記録は最初の変更時の 1 回だけ
    bool keyMovePushed_ = false;      ///< キーでの移動の記録も、連なりの最初の 1 回だけ
    bool textEditPushed_ = false;     ///< テキスト編集中の記録も同様
    EditSnapshot textEditBefore_;     ///< 上で積む編集前のスナップショット
};

}  // namespace blinker
