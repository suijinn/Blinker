#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/edit_style.h"
#include "core/keymap.h"
#include "core/sort_order.h"
#include "core/text_edit.h"
#include "core/text_style.h"
#include "platform/annotation.h"

/**
 * @file menu.h
 * @brief ポップアップメニューの組み立て ― 状態から項目の木と末端項目の一覧を作る。
 *
 * OS ヘッダに依存しない(単体テスト対象)。**メニューを出さないし、選ばれた結果も
 * 適用しない** ― 表示 (IAppHost::showContextMenu) と適用は App の担当で、ここは
 * 「今の状態ならどんな項目が並ぶか」だけを答える純関数の集まり。
 *
 * どの build 関数も、項目の木 (MenuItem) と末端項目の一覧 (entries) を対にして返す。
 * showContextMenu が返す index が entries の添字になる ― ウィンドウ層は末端へ
 * 深さ優先で通し番号を振るので、entries も同じ順に積むこと(区切り線は数えない)。
 */

namespace blinker {

/**
 * @brief ポップアップメニューの 1 項目。
 *
 * children が空でなければサブメニューになる。separator = true の項目は区切り線
 * (text・children は無視される)。
 */
struct MenuItem {
    std::string text;                 ///< 表示文字列(UTF-8)
    bool checked = false;             ///< チェックマークを付けるか
    bool separator = false;           ///< 区切り線として扱うか
    std::vector<MenuItem> children;   ///< サブメニューの項目(空なら末端項目)
};

/// @brief 区切り線の項目を作る。
/// @return separator = true の項目。
MenuItem menuSeparator();

/**
 * @brief メニューに出す画像の大きさ(ピクセル)。
 */
struct MenuImageSize {
    uint32_t width = 0;   ///< 幅
    uint32_t height = 0;  ///< 高さ
};

/**
 * @brief フォント選択メニューの 1 候補。
 */
struct FontFamilyChoice {
    std::string label;   ///< 表示ラベル(「游ゴシック」など。UTF-8)
    std::string family;  ///< 描画側へ渡すフォントファミリ名(UTF-8)
};

/// @brief フォント選択メニューの候補一覧。
using FontFamilyChoices = std::vector<FontFamilyChoice>;

/**
 * @brief フォントファミリが使えるかを答える述語。
 *
 * 描画側への問い合わせ (IAnnotationRasterizer::hasFontFamily) を差し込む口。
 * ここを関数で受けることで、メニューの組み立てが描画側を知らずに済む。
 */
using FontAvailableFn = std::function<bool(const std::string&)>;

/**
 * @brief 実際に描画に使われるフォント名を返す。
 * @param[in] family フォントファミリ名(UTF-8)。
 * @return family。空なら既定フォント (kDefaultFontFamily)。
 * @note 引数の std::string から一時オブジェクトを作らないよう view で通す。
 */
std::string_view effectiveFontFamily(std::string_view family);

/**
 * @brief フォント選択メニューに並べる候補を求める。
 *
 * 定義済みの候補のうち、available が「使える」と答えたものだけを返す。
 * current が候補に無い場合(ini で任意のフォントを指定した場合など)は
 * 末尾に足して、選び直したあとでも戻れるようにする。
 *
 * @param[in] current   現在のフォントファミリ名(UTF-8)。空なら既定フォント。
 * @param[in] available フォントが使えるかを答える述語。
 * @return 候補の一覧。並びは定義順で、現在のフォントを足す場合だけ末尾。
 */
FontFamilyChoices fontFamilyChoices(std::string_view current, const FontAvailableFn& available);

/**
 * @brief ツール切り替えメニューの末端項目が表す操作。
 * @note 設定系(SelectTool 以外)はメニューを再表示して続けて選択できる。
 */
struct EditMenuEntry {
    /// @brief 末端項目が表す操作の種類。
    enum class Action {
        SelectTool, StrokeWidth, FontSize, FontFamily, PickColor,
        FillAlpha, PickFillColor, BorderWidth, PickBorderColor,
        ResizePercent,  ///< 画像を value % にリサイズする(縦横比を保つ)
        ResizeLongEdge  ///< 画像の長辺を value px にリサイズする(縦横比を保つ)
    };
    Action action;                    ///< 操作の種類
    EditTool tool = EditTool::Rect;   ///< SelectTool で選ばれたツール
    float value = 0;                  ///< 設定系の値
    std::string family;               ///< FontFamily で選ばれたフォント名(UTF-8)
};

/// @brief サイドバー(ファイル名一覧)を右クリックしたときのメニューの末端項目。
struct SidebarMenuEntry {
    /// @brief 末端項目が表す操作の種類。
    enum class Action { SortKey, SortAscending, SortDescending, ToggleRecursive };
    Action action;                ///< 操作の種類
    SortKey key = SortKey::Name;  ///< SortKey で選ばれた並び替えキー
};

/// @brief 注釈オブジェクトを右クリックしたときのメニューの末端項目。
struct ObjectMenuEntry {
    /// @brief 末端項目が表す操作の種類。
    enum class Action {
        EditText, Delete, Angle, StrokeWidth, StrokeAlpha, FontSize, FontFamily, PickColor,
        FillAlpha, PickFillColor, BorderWidth, PickBorderColor, Number
    };
    Action action;       ///< 操作の種類
    /// Angle/StrokeWidth/StrokeAlpha/FontSize/FillAlpha/BorderWidth/Number の値
    float value = 0;
    std::string family;  ///< FontFamily で選ばれたフォント名(UTF-8)
};

/**
 * @brief ツール切り替えメニューの構造を組み立てる。
 * @param[in]  style     現在の編集設定(ツールと見た目。チェックの位置と見出しに使う)。
 * @param[in]  keymap    キー割り当て(項目にアクセラレータ表記を添えるために引く)。
 * @param[in]  available フォントが使えるかを答える述語。
 * @param[in]  image     表示中の画像の大きさ。std::nullopt なら「画像をリサイズ」を出さない。
 * @param[out] entries   末端項目の一覧。entries[i] が showContextMenu の返す index i に対応する。
 * @return メニュー構造。現在のツールと現在の設定値にチェックが付く。
 */
std::vector<MenuItem> buildEditMenu(const EditStyle& style, const Keymap& keymap,
                                    const FontAvailableFn& available,
                                    std::optional<MenuImageSize> image,
                                    std::vector<EditMenuEntry>& entries);

/**
 * @brief リサイズのプリセットメニューを組み立てる。
 * @param[in]  image   表示中の画像の大きさ(変換後の大きさを求めるのに使う)。
 * @param[out] entries 末端項目の一覧。entries[i] が showContextMenu の返す index i に対応する。
 * @return メニュー構造(倍率と長辺の 2 つのサブメニュー)。項目には変換後の
 *         大きさを添える(選ぶ前に結果が分かるように)。
 * @note ツール切り替えメニューへ埋め込むためにも呼ばれるので、entries は
 *       呼び出し側の並びに続けて積む(深さ優先の通し番号を保つ)。
 */
std::vector<MenuItem> buildResizeMenu(MenuImageSize image, std::vector<EditMenuEntry>& entries);

/**
 * @brief サイドバー(ファイル名一覧)のメニュー構造を組み立てる。
 * @param[in]  order     現在の並び順。
 * @param[in]  recursive サブフォルダを含めているか。
 * @param[in]  keymap    キー割り当て(項目にアクセラレータ表記を添えるために引く)。
 * @param[out] entries   末端項目の一覧。entries[i] が showContextMenu の返す index i に対応する。
 * @return メニュー構造。現在の並び順・向き・再帰の有無にチェックが付く。
 */
std::vector<MenuItem> buildSidebarMenu(SortOrder order, bool recursive, const Keymap& keymap,
                                       std::vector<SidebarMenuEntry>& entries);

/**
 * @brief 注釈オブジェクトのメニュー構造を組み立てる。
 * @param[in]  spec      対象の注釈。種別によって出す項目が変わり、値は見出しとチェックに出る。
 * @param[in]  keymap    キー割り当て(項目にアクセラレータ表記を添えるために引く)。
 * @param[in]  available フォントが使えるかを答える述語。
 * @param[out] entries   末端項目の一覧。entries[i] が showContextMenu の返す index i に対応する。
 * @return メニュー構造。
 */
std::vector<MenuItem> buildObjectMenu(const AnnotationSpec& spec, const Keymap& keymap,
                                      const FontAvailableFn& available,
                                      std::vector<ObjectMenuEntry>& entries);

/**
 * @brief 書式メニューの先頭に並ぶトグル項目。
 */
struct TextStyleFlagItem {
    std::string_view text;  ///< 表示文字列(UTF-8)
    TextStyleFlag flag;     ///< トグルする属性
};

/// @brief 書式メニューのトグル項目。末端 index 0 以降がこの並びに対応する。
inline constexpr std::array<TextStyleFlagItem, 3> kTextStyleFlags{{
    {"太字", TextStyleFlag::Bold},
    {"斜体", TextStyleFlag::Italic},
    {"下線", TextStyleFlag::Underline},
}};

/**
 * @brief 編集中テキストの書式メニューと、選択結果を読み解くための添字。
 *
 * 末端項目が種類ごとに構造の違う操作(トグル・フォント・色ダイアログ)なので、
 * 他のメニューのような entries ではなく index の境目を返す。
 */
struct TextStyleMenu {
    std::vector<MenuItem> items;  ///< メニュー構造
    FontFamilyChoices families;   ///< フォント候補(選ばれた index から名前を引くのに使う)
    std::string wholeFamily;      ///< 注釈全体のフォント(実際に描画に使われる名前)
    uint32_t initialColor = 0;    ///< 文字色ダイアログの初期値(0xRRGGBB)
    size_t familyBase = 0;        ///< フォント候補の先頭に対応する末端 index
    size_t colorIndex = 0;        ///< 文字色に対応する末端 index
};

/**
 * @brief 編集中テキストの選択部分に対する書式メニューを組み立てる。
 *
 * 色・フォントを指定していない範囲は注釈全体のもので描かれるので、そちらを
 * 見出しと初期値に使う。
 *
 * @param[in] spec      編集中のテキスト注釈(全体の色とフォントを引く)。
 * @param[in] buffer    編集中のテキスト(選択範囲の書式を引く)。
 * @param[in] available フォントが使えるかを答える述語。
 * @return メニュー構造と、選択結果を読み解くための添字。
 * @pre buffer に選択範囲があること(無い場合の見た目は保証しない)。
 */
TextStyleMenu buildTextStyleMenu(const AnnotationSpec& spec, const TextEditBuffer& buffer,
                                 const FontAvailableFn& available);

}  // namespace blinker
