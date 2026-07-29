#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "core/edit_style.h"
#include "platform/decoder.h"

/**
 * @file view_text.h
 * @brief 常に見えている表示文字列(タイトルバー・ステータスバー)の組み立て。
 *
 * どれも「状態 → 文字列」の純粋な変換で、`IAppHost` にも描画にも触らない。
 * 設定するのは呼び出し側 (`App::updateTitle` / `App::statusBar`) の役目。
 *
 * OS ヘッダに依存しない(単体テスト対象)。
 */

namespace blinker {

/**
 * @brief タイトルバーの組み立てに要る状態。
 *
 * どれも App が持っている値をそのまま写したもの。`loading` だけは
 * 「表示中の画像が一覧の現在位置と違う」という導出値なので App 側で判断する。
 */
struct TitleState {
    std::string_view appName;    ///< アプリ名と版("Blinker v0.1.5 (abc1234)")
    std::string_view filename;   ///< 表示中のファイル名(UTF-8)
    size_t index = 0;            ///< 一覧の中の位置(0 起点。表示は +1 する)
    size_t count = 0;            ///< 一覧の枚数。0 なら一覧が空
    float zoom = 1.0f;           ///< 表示倍率(1.0 = 等倍)
    bool fromClipboard = false;  ///< 貼り付けた画像を表示しているか
    bool failed = false;         ///< 読み込みに失敗したか
    bool loading = false;        ///< デコード待ち(前の画像を表示したまま)か
    bool edited = false;         ///< 未保存の編集があるか
};

/**
 * @brief ウィンドウタイトルを組み立てる。
 *
 * 通常は `ファイル名 [i/n] ズーム% - アプリ名`。読み込み中・失敗中はズーム率の代わりに
 * その旨を出す(まだ / もう表示していない画像の倍率を出しても意味がないため)。
 *
 * @param[in] state 表示中の画像と一覧の状態。
 * @return タイトル文字列(UTF-8)。一覧が空で貼り付け画像も無ければアプリ名だけ。
 */
std::string windowTitle(const TitleState& state);

/**
 * @brief 多フレーム画像(アニメ GIF・多ページ TIFF・ICO)のフレーム情報。
 */
struct FrameStatus {
    std::string_view unitLabel;  ///< 数え方の呼び名("フレーム" / "ページ")
    std::string_view label;      ///< そのフレームの表示名(ICO の "256 x 256" 等。空なら出さない)
    size_t index = 0;            ///< 現在のフレーム(0 起点。表示は +1 する)
    size_t count = 0;            ///< フレーム数
    bool animation = false;      ///< 再生できる(アニメーションの)フレーム列か
    bool playing = false;        ///< 再生中か
};

/**
 * @brief ステータスバー左側の組み立てに要る状態。
 */
struct StatusTextState {
    std::string_view message;             ///< 通知メッセージ。空でなければこれだけを出す
    const DecodedImage* image = nullptr;  ///< 表示中の画像。無ければ nullptr
    std::optional<FrameStatus> frame;     ///< フレームが複数あるときだけ入れる
    std::string_view error;               ///< 読み込み失敗の理由(空なら理由なし)
    EditTool tool = EditTool::Rect;       ///< 現在の編集ツール
    bool recursive = false;               ///< サブフォルダを含めて一覧しているか
    bool failed = false;                  ///< 読み込みに失敗したか
};

/**
 * @brief ステータスバー左側の文字列を組み立てる。
 *
 * 通知メッセージがあればそれだけを出し、無ければ画像の情報(大きさ・フレーム位置・
 * 再帰の有無・現在のツール)を `  |  ` 区切りで並べる。
 *
 * @param[in] state 表示中の画像と通知の状態。
 * @return 表示する文字列(UTF-8)。画像も通知も失敗も無ければ空文字列。
 */
std::string statusText(const StatusTextState& state);

/**
 * @brief 画像の 1 画素の座標と色を表す文字列を組み立てる(ステータスバー右側)。
 *
 * 事前乗算を解いてから出すので、半透明の画素でも元の色が読める。不透明なら
 * `RGB(...)`、そうでなければ `RGBA(...)` を添える。
 *
 * @param[in] image 対象の画像(32bpp PBGRA)。
 * @param[in] x     画像座標の x(範囲外なら空文字列を返す)。
 * @param[in] y     画像座標の y(範囲外なら空文字列を返す)。
 * @return 座標と色を表す文字列(UTF-8)。範囲外なら空文字列。
 */
std::string pixelInfoText(const DecodedImage& image, int x, int y);

} // namespace blinker
