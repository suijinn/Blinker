#pragma once

#include <windows.h>

// WinRT を C++/WinRT ではなく ABI (MIDL 生成ヘッダ) で直接使うための下ごしらえ。
// windowsapp.lib への静的リンクを避け、exe のインポートを増やさない
#include <roapi.h>
#include <winstring.h>
#include <wrl/client.h>

#include <string>
#include <string_view>

#include "core/unicode.h"

/**
 * @file winrt_abi.h
 * @brief WinRT を ABI で呼ぶための共通部品(combase の遅延解決・HSTRING・ファクトリ取得)。
 *
 * 文字認識 (ocr_winrt) と印刷 (print_winrt) が共用する。
 */

namespace blinker {

/**
 * @brief combase.dll から遅延解決した関数の一覧。
 *
 * 静的リンク (windowsapp.lib) を避けることで、exe のインポートテーブルが増えず、
 * WinRT を使う機能(文字認識・印刷)を一度も使わなければ combase.dll が
 * 読み込まれることもない(起動時間を守る)。
 */
struct ComBaseApi {
    /// @cond
    // 各メンバは combase.dll の同名の API へのポインタ。Doxygen は関数ポインタのメンバを
    // 関数と見なして @param / @return を要求するが、引数名を持てないので書きようがない。
    // ここだけ文書化の対象から外す
    HRESULT(WINAPI* roInitialize)(RO_INIT_TYPE) = nullptr;
    void(WINAPI* roUninitialize)() = nullptr;
    HRESULT(WINAPI* roGetActivationFactory)(HSTRING, REFIID, void**) = nullptr;
    HRESULT(WINAPI* windowsCreateString)(PCNZWCH, UINT32, HSTRING*) = nullptr;
    HRESULT(WINAPI* windowsDeleteString)(HSTRING) = nullptr;
    PCWSTR(WINAPI* windowsGetStringRawBuffer)(HSTRING, UINT32*) = nullptr;
    /// @endcond

    bool ok = false;  ///< すべて解決できたか。false なら WinRT は使えない
};

/**
 * @brief combase.dll の関数群を返す(初回呼び出し時に解決する)。
 * @return 解決結果。Windows 8 より前など combase.dll が無い環境では ok == false。
 */
const ComBaseApi& comBase();

/// @brief HSTRING の所有権を持つ薄いラッパ(WindowsCreateString / WindowsDeleteString の対)。
class HString {
public:
    HString() = default;

    /**
     * @brief 文字列から HSTRING を作る。
     * @param[in] s 元の文字列(UTF-16)。
     */
    explicit HString(std::wstring_view s) {
        if (!comBase().ok) return;
        comBase().windowsCreateString(s.data(), static_cast<UINT32>(s.size()), &value_);
    }

    ~HString() {
        if (value_ && comBase().ok) comBase().windowsDeleteString(value_);
    }

    HString(const HString&) = delete;
    HString& operator=(const HString&) = delete;

    /**
     * @brief 保持している HSTRING を返す。
     * @return HSTRING。作成に失敗していれば nullptr(空文字列として扱われる)。
     */
    HSTRING get() const { return value_; }

    /**
     * @brief 受け取り用のポインタを返す(WinRT API の [out] 引数に渡す)。
     * @return 内部の HSTRING へのポインタ。
     */
    HSTRING* put() { return &value_; }

    /**
     * @brief 保持している文字列を UTF-8 で返す。
     * @return UTF-8 の文字列。空・未設定なら空文字列。
     */
    std::string toUtf8() const {
        if (!value_ || !comBase().ok) return {};
        UINT32 length = 0;
        const wchar_t* buffer = comBase().windowsGetStringRawBuffer(value_, &length);
        if (!buffer) return {};
        return wideToUtf8(std::wstring_view(buffer, length));
    }

private:
    HSTRING value_ = nullptr;  ///< 所有している HSTRING
};

/**
 * @brief ランタイムクラスのアクティベーションファクトリを取得する。
 * @param[in]  classId ランタイムクラス名(windows.*.h の RuntimeClass_* 定数)。
 * @param[out] out     受け取り先。
 * @return 成功なら S_OK。combase が使えない環境では E_NOINTERFACE。
 */
template <typename T>
HRESULT activationFactory(const wchar_t* classId, Microsoft::WRL::ComPtr<T>& out) {
    if (!comBase().ok) return E_NOINTERFACE;
    const HString name(classId);
    if (!name.get()) return E_OUTOFMEMORY;
    return comBase().roGetActivationFactory(name.get(), __uuidof(T),
                                            reinterpret_cast<void**>(out.GetAddressOf()));
}

} // namespace blinker
