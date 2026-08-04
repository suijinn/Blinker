#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "core/config.h"
#include "platform/annotation.h"

/**
 * @file edit_style.h
 * @brief これから描く注釈の設定 ― どのツールで、どんな色・太さ・文字で描くか。
 *
 * OS ヘッダに依存しない(単体テスト対象)。**画像も注釈の一覧も窓も知らない** ―
 * 次に作る 1 件へ写す値だけを持つ。既に置いた注釈の書式変更(選択範囲の色・太字など)は
 * 対象の AnnotationSpec を直接触るので、ここは通らない。
 */

namespace blinker {

/**
 * @brief 編集ドラッグで実行する編集ツール。
 *
 * 事前に選んだツールが編集ドラッグ(既定では右ドラッグ。MouseRole::Edit の
 * ボタン)で即座に適用される(ドラッグ中はプレビューが出る)。
 * 切り替えは注釈のない場所での右クリック(ドラッグなし)で開くメニュー、または
 * Command::SelectToolRect 以降のコマンドで行う。
 *
 * @note トリミングはツールではない。範囲は Rect の注釈オブジェクトとして作り、
 *       切り出しはその右クリックメニュー(Command::CropToSelection)から行う
 *       ―― ドラッグを離した時点で確定させず、ハンドルで微調整できるようにするため。
 */
enum class EditTool {
    Rect,     ///< 矩形を描く
    Ellipse,  ///< 楕円を描く
    Arrow,    ///< 矢印を描く
    Line,     ///< 直線を描く
    Pen,      ///< ドラッグの軌跡をそのまま線にする(手書き)
    Marker,   ///< 手書きの半透明・太線版(蛍光ペン)
    Number,   ///< 連番の数字入りマーカーを置く(番号は自動で増える)
    Text,     ///< テキストボックスを作り、その場で入力を始める
    Ocr,      ///< 選択した範囲の文字を認識してクリップボードへコピーする(画像は変えない)
};

/**
 * @brief ツールの表示名を返す。
 * @param[in] tool 対象のツール。
 * @return 「トリミング」のような表示名(UTF-8)。メニューとステータスバーで共通。
 */
std::string_view toolLabel(EditTool tool);

/**
 * @brief 新規注釈の設定(選択中のツールと、その見た目)。
 *
 * ツールも同じ場所に置くのは、何で描くかが見た目の一部だから
 * (マーカーは「線の太さ」を太く半透明にしたペン)。設定はメニューと blinker.ini の
 * `[edit]` から入り、EditStyle::applyTo で新規注釈へ写される。
 *
 * 線幅・文字サイズ・枠線幅はすべて画像座標で、表示倍率では変わらない
 * (同じ設定で描いたものが、描いたときのズームによらず同じ太さになるように)。
 *
 * @note UI スレッド専用(App と同じ)。スレッド安全ではない。
 */
class EditStyle {
public:
    /// @brief 現在のツールを返す。
    /// @return 編集ドラッグで実行されるツール。
    EditTool tool() const { return tool_; }

    /**
     * @brief 現在のツールを切り替える。
     * @param[in] tool 切り替え先のツール。
     */
    void setTool(EditTool tool) { tool_ = tool; }

    /**
     * @brief 現在のツールが手書き(ペン・マーカー)かを返す。
     * @return 手書きなら true。編集ドラッグ中に軌跡を溜めるかの判定に使う。
     */
    bool penActive() const;

    /// @brief 新規注釈の色を返す。
    /// @return 0xRRGGBB 形式の色値。
    uint32_t colorRGB() const { return colorRGB_; }

    /**
     * @brief 新規注釈の色を設定する。
     * @param[in] rgb 0xRRGGBB 形式の色値。
     */
    void setColorRGB(uint32_t rgb) { colorRGB_ = rgb; }

    /// @brief 新規注釈の線幅を返す。
    /// @return 線幅(画像座標)。
    float strokeWidth() const { return strokeWidth_; }

    /**
     * @brief 新規注釈の線幅を設定する。
     * @param[in] px 線幅(画像座標)。1〜100 の範囲へ丸められる。
     */
    void setStrokeWidth(float px);

    /// @brief 新規テキストのフォントサイズを返す。
    /// @return フォントサイズ(画像座標)。
    float fontSize() const { return fontSize_; }

    /**
     * @brief 新規テキストのフォントサイズを設定する。
     * @param[in] px フォントサイズ(画像座標)。6〜200 の範囲へ丸められる。
     */
    void setFontSize(float px);

    /// @brief 新規テキストのフォントファミリ名を返す。
    /// @return ファミリ名(UTF-8)。常に非空。
    const std::string& fontFamily() const { return fontFamily_; }

    /**
     * @brief 新規テキストのフォントファミリ名を設定する。
     * @param[in] family ファミリ名(UTF-8)。空なら何もしない(既定へは戻さない)。
     * @note 存在確認はしない。入っていなければ描画側がフォールバックする。
     */
    void setFontFamily(std::string family);

    /// @brief 新規注釈の塗りつぶし色を返す。
    /// @return 0xRRGGBB 形式の色値。
    uint32_t fillRGB() const { return fillRGB_; }

    /**
     * @brief 新規注釈の塗りつぶし色を設定する。
     * @param[in] rgb 0xRRGGBB 形式の色値。
     * @note 塗りなし(不透明度 0)なら不透明にする。色を選んだのに塗られないままに
     *       ならないようにするため。既に塗っているなら不透明度はそのまま。
     */
    void setFillColor(uint32_t rgb);

    /// @brief 新規注釈の塗りつぶしの不透明度を返す。
    /// @return 0〜255。0 は塗りなし。
    int fillAlpha() const { return fillAlpha_; }

    /**
     * @brief 新規注釈の塗りつぶしの不透明度を設定する。
     * @param[in] alpha 不透明度。0〜255 の範囲へ丸められる。0 は塗りなし。
     */
    void setFillAlpha(int alpha);

    /// @brief 新規テキストの枠線色を返す。
    /// @return 0xRRGGBB 形式の色値。
    uint32_t borderRGB() const { return borderRGB_; }

    /**
     * @brief 新規テキストの枠線色を設定する。
     * @param[in] rgb 0xRRGGBB 形式の色値。
     * @note 枠線なし(幅 0)なら 1px にする(setFillColor と同じ理由)。
     */
    void setBorderColor(uint32_t rgb);

    /// @brief 新規テキストの枠線幅を返す。
    /// @return 枠線幅(画像座標)。0 は枠線なし。
    float borderWidth() const { return borderWidth_; }

    /**
     * @brief 新規テキストの枠線幅を設定する。
     * @param[in] px 枠線幅(画像座標)。0〜100 の範囲へ丸められる。0 は枠線なし。
     */
    void setBorderWidth(float px);

    /**
     * @brief 新規注釈へ設定を写す。
     * @param[in,out] spec 書き込み先。kind を設定してから渡すこと。
     * @pre spec.kind が確定していること(マーカーの判定に使う)。
     * @note 位置(p1/p2・points)と種別ごとの値(連番の番号など)は呼び出し側が入れる。
     *       手書きでツールがマーカーなら、線幅を太く・半透明にしたものを書き込む。
     */
    void applyTo(AnnotationSpec& spec) const;

    /**
     * @brief blinker.ini の `[edit]` セクションを適用する。
     * @param[in] config 読み込み済みの設定。
     * @note 範囲外の値は各セッターと同じ範囲へ丸める。未指定のキーは現在値のまま。
     *       `tool` は未指定・未知の名前なら無視する。
     */
    void applyConfig(const Config& config);

private:
    /// マーカーの線幅の倍率(「線の太さ」設定に掛ける)。蛍光ペンらしい太さにする
    static constexpr float kMarkerWidthScale = 4.0f;
    static constexpr int kMarkerAlpha = 102;  ///< マーカーの線の不透明度(0-255。約 40%)

    EditTool tool_ = EditTool::Rect;  ///< 編集ドラッグで実行するツール
    uint32_t colorRGB_ = 0xFF3B30;  ///< 新規注釈の色(0xRRGGBB)
    float strokeWidth_ = 3.0f;      ///< 新規注釈の線幅(画像座標)
    float fontSize_ = 18.0f;        ///< 新規テキストのフォントサイズ(画像座標)
    std::string fontFamily_ = kDefaultFontFamily;  ///< 新規テキストのフォントファミリ名(UTF-8)
    uint32_t fillRGB_ = 0xFFFFFF;   ///< 新規注釈の塗りつぶし色(0xRRGGBB)
    int fillAlpha_ = 0;             ///< 塗りつぶしの不透明度(0-255)。0 は塗りなし
    uint32_t borderRGB_ = 0x000000; ///< 新規テキストの枠線色(0xRRGGBB)
    float borderWidth_ = 0;         ///< 新規テキストの枠線幅(画像座標)。0 は枠線なし
};

}  // namespace blinker
