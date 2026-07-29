#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

/**
 * @file decoder.h
 * @brief デコード済み画像の表現と、画像デコーダのプラットフォーム抽象。
 */

namespace blinker {

/**
 * @brief デコード済み画像。
 *
 * ピクセルは 32bit BGRA(アルファ事前乗算)、stride = width * 4。
 */
struct DecodedImage {
    uint32_t width = 0;            ///< 幅(ピクセル)
    uint32_t height = 0;           ///< 高さ(ピクセル)
    std::vector<uint8_t> pixels;   ///< ピクセルデータ(32bpp PBGRA、上から下へ)
    uint32_t sourceWidth = 0;      ///< 縮小して取り込んだ場合の元ファイルの幅(0 = 等倍)
    uint32_t sourceHeight = 0;     ///< 縮小して取り込んだ場合の元ファイルの高さ(0 = 等倍)
    bool colorConverted = false;   ///< 埋め込みプロファイルから sRGB へ変換したか
    /// 変換できるプロファイルがあるが、まだ変換していない(遅延カラーマネジメント用の
    /// 内部ヒント。これが立っていると ImageCache が後から読み直す)
    bool colorPending = false;

    /**
     * @brief ピクセルデータのバイト数を返す。
     * @return pixels のサイズ(バイト)。
     */
    size_t byteSize() const { return pixels.size(); }

    /**
     * @brief 元ファイルより小さく取り込まれた画像かを返す。
     *
     * true のとき、このピクセルを保存すると元より小さい画像になる。上書き保存は
     * 元データを失うので拒否する(App::executeSaveOverwrite)。
     *
     * @return 縮小して取り込まれていれば true。
     */
    bool downscaled() const { return sourceWidth != 0 || sourceHeight != 0; }
};

/**
 * @brief 1 ファイルが持つフレームの種別。
 *
 * 「フレーム間に依存があるか」が実装方針を分ける軸になっている。Pages は 1 枚だけ
 * 独立してデコードできるので必要になったページだけ読み、Animation は前フレームの
 * 合成結果の上に差分を重ねる形なので全フレームをまとめてデコードする。
 */
enum class SequenceKind {
    Single,     ///< 単一フレーム(通常の画像)
    Pages,      ///< 独立してデコードできる複数ページ(多ページ TIFF、ICO の各サイズ)
    Animation,  ///< 前フレームに依存するアニメーション(GIF)
};

/**
 * @brief フレーム列の 1 コマ。
 */
struct FrameEntry {
    std::shared_ptr<DecodedImage> image;  ///< 画素。未デコードなら nullptr(Pages のみ)
    uint32_t delayMs = 0;                 ///< 次のフレームまでの時間(Animation のみ)
    std::string label;  ///< 表示名(ICO の "256 x 256" 等)。空ならページ番号だけ出す
};

/**
 * @brief 1 ファイルが持つフレームの並び。
 *
 * **frames[0] は必ず IImageDecoder::decode が返すものと同じ絵にすること。**
 * 並び順はデコーダが決めてよく(ICO は大きい順)、App から見た index はこの並びを指す。
 *
 * frames が shared_ptr の配列なので複製は安い。ImageCache はフレームが 1 枚増えるたびに
 * この構造体を作り直して差し替える(コピーオンライト)ため、UI スレッドは古い
 * shared_ptr をロックなしで読み続けられる。
 */
struct ImageSequence {
    SequenceKind kind = SequenceKind::Single;  ///< フレームの種別
    std::vector<FrameEntry> frames;            ///< フレーム(常に 1 要素以上)
    int loopCount = 0;        ///< 繰り返し回数。0 = 無限(Animation のみ)
    bool truncated = false;   ///< 予算超過で全フレームを持てず静止画へ落としたか

    /**
     * @brief デコード済みフレームのピクセル合計バイト数を返す。
     * @return 各フレームの byteSize() の総和(未デコードのフレームは 0)。
     */
    size_t byteSize() const {
        size_t total = 0;
        for (const FrameEntry& f : frames) {
            if (f.image) total += f.image->byteSize();
        }
        return total;
    }
};

/**
 * @brief フレーム構成の調査結果(画素は含まない)。
 */
struct SequenceInfo {
    SequenceKind kind = SequenceKind::Single;  ///< フレームの種別
    /// フレーム数。Pages では正確な数(この数だけページの枠が用意される)。
    /// Animation では「2 以上であること」だけが意味を持ち、実数は decodeAnimation が決める
    uint32_t frameCount = 1;
    int loopCount = 0;                         ///< 繰り返し回数。0 = 無限
    /// 各フレームの表示名。空、または frameCount 個(ICO のサイズ表記など)
    std::vector<std::string> labels;
};

/**
 * @brief アニメーションを展開するときの上限([animation] max_memory_mb / max_frames)。
 *
 * 合成後のフレームは毎回フルキャンバスなので、上限を超えるものは静止画として扱う。
 * 途中まで再生すると「途中で止まるアニメ」にしか見えないため、中途半端には持たない。
 */
struct AnimationLimits {
    size_t maxBytes = size_t{256} << 20;  ///< 展開後のピクセル合計の上限(バイト)
    uint32_t maxFrames = 1000;            ///< フレーム数の上限
};

/**
 * @brief 画像デコーダのプラットフォーム抽象。
 *
 * Windows 実装は WIC (decoder_wic)、SDL バックエンドは stb_image (decoder_stb)。
 * ImageCache のワーカースレッドから呼ばれるため、実装はスレッド安全にすること。
 */
class IImageDecoder {
public:
    virtual ~IImageDecoder() = default;

    /**
     * @brief 画像ファイルをデコードする。
     * @param[in]  path  デコードする画像のパス。
     * @param[out] error 非 nullptr のとき、失敗した場合に限り原因を表す短い UTF-8 文字列
     *                   (失敗した段階とプラットフォーム固有のエラーコード。例:
     *                   `"PBGRA変換 (0x88982F50)"`)が入る。成功時は変更しない。
     *                   ステータスバーへそのまま表示される想定。
     * @return デコード結果(32bpp PBGRA)。失敗時は nullptr。
     */
    virtual std::shared_ptr<DecodedImage> decode(const std::filesystem::path& path,
                                                 std::string* error = nullptr) = 0;

    /**
     * @brief 埋め込みプロファイルを sRGB へ変換して読み直す(遅延カラーマネジメント)。
     *
     * `decode` が `DecodedImage::colorPending` を立てて返したとき、ImageCache が
     * 手すきに一度だけ呼ぶ。色変換は 24MP の写真で 0.5 秒ほどかかるため、最初の
     * 表示では変換せず、後からこの結果へ差し替えることで待ち時間を隠している。
     *
     * @param[in]  path  読み直す画像のパス。
     * @param[out] error 非 nullptr のとき、失敗した場合に理由が入る。
     * @return 変換済みの画像。変換が不要・未対応・失敗した場合は nullptr
     *         (呼び出し側は最初の結果を使い続け、二度目は試さない)。
     * @note 既定の実装は nullptr を返す(カラーマネジメント非対応のデコーダ用)。
     */
    virtual std::shared_ptr<DecodedImage> decodeColorManaged(const std::filesystem::path& path,
                                                             std::string* error = nullptr) {
        (void)path;
        (void)error;
        return nullptr;
    }

    /**
     * @brief フレーム構成を調べる(画素は読まない)。
     *
     * ImageCache が `mayHaveMultipleFrames` で絞ったパスに対してだけ呼ぶ
     * (通常の写真でファイルを二度開くと起動が遅くなるため)。
     *
     * @param[in] path 調べる画像のパス。
     * @return フレーム構成。単一フレーム・調べられない場合は既定値(Single / 1 枚)。
     * @note 既定の実装は常に Single を返す(多フレーム非対応のデコーダ用)。
     */
    virtual SequenceInfo probeSequence(const std::filesystem::path& path) {
        (void)path;
        return {};
    }

    /**
     * @brief 独立してデコードできるページを 1 枚読む(SequenceKind::Pages 専用)。
     * @param[in]  path  デコードする画像のパス。
     * @param[in]  index probeSequence が返した並びでのページ番号(0 起点)。
     * @param[out] error 非 nullptr のとき、失敗した場合に理由が入る。
     * @return デコード結果(32bpp PBGRA)。失敗時は nullptr。
     * @note 既定の実装は nullptr を返す。
     */
    virtual std::shared_ptr<DecodedImage> decodePage(const std::filesystem::path& path,
                                                     uint32_t index, std::string* error) {
        (void)path;
        (void)index;
        (void)error;
        return nullptr;
    }

    /**
     * @brief アニメーションの全フレームを合成してデコードする(SequenceKind::Animation 専用)。
     * @param[in]  path   デコードする画像のパス。
     * @param[in]  limits 展開後の大きさとフレーム数の上限。
     * @param[out] out    成功時に全フレーム・遅延時間・ループ回数が入る。
     * @param[out] error  非 nullptr のとき、失敗した場合に理由が入る。
     * @return 全フレームを展開できたら true。上限超過・失敗時は false
     *         (呼び出し側は静止画のまま扱う)。
     * @note false を返す場合、**上限を超えたときだけ** `out.truncated` を立てること。
     *       呼び出し側はこれを見て「多すぎるので静止画にした」と案内する。
     *       アニメーションでなかった・読めなかっただけなら立てない(黙って静止画にする)。
     * @note 既定の実装は false を返す。
     */
    virtual bool decodeAnimation(const std::filesystem::path& path, const AnimationLimits& limits,
                                 ImageSequence& out, std::string* error) {
        (void)path;
        (void)limits;
        (void)out;
        (void)error;
        return false;
    }
};

} // namespace blinker
