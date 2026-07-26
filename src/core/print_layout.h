#pragma once

/**
 * @file print_layout.h
 * @brief 用紙への画像の配置計算(純関数)。
 *
 * OS ヘッダに依存しない(単体テスト対象)。
 */

namespace blinker {

/**
 * @brief 用紙上での画像の置き場所。
 *
 * 座標・大きさは印刷可能領域(余白を引いた後の領域)の左上を原点とする
 * デバイスピクセル。
 */
struct PrintPlacement {
    int x = 0;             ///< 左端
    int y = 0;             ///< 上端
    int width = 0;         ///< 幅。0 なら配置できなかった(入力が不正)
    int height = 0;        ///< 高さ。0 なら配置できなかった(入力が不正)
    bool rotated = false;  ///< 画像を時計回りに 90 度回してから置くか
};

/**
 * @brief 縦横比を保ったまま領域いっぱいに収まる配置を求める。
 *
 * 画像より領域が大きければ拡大する(用紙に対して原寸で刷ると、600dpi の
 * プリンタでは切手ほどの大きさにしかならないため)。位置は領域の中央。
 *
 * @param[in] imageWidth  画像の幅(ピクセル)。
 * @param[in] imageHeight 画像の高さ(ピクセル)。
 * @param[in] areaWidth   置ける領域の幅(デバイスピクセル)。
 * @param[in] areaHeight  置ける領域の高さ(デバイスピクセル)。
 * @param[in] autoRotate  true なら、90 度回した方が大きく刷れる場合に回す
 *                        (横長の写真を縦向きの用紙へ刷るときなど)。
 * @return 配置。いずれかの引数が 0 以下なら width / height が 0 のもの
 *         (呼び出し側は印刷を諦めること)。
 */
PrintPlacement layoutPrintImage(int imageWidth, int imageHeight, int areaWidth, int areaHeight,
                                bool autoRotate);

} // namespace blinker
