#include "win/drag_drop_win.h"

#include <windows.h>

#include <objidl.h>
#include <shlobj.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <system_error>

namespace blinker {
namespace {

/**
 * @brief DoDragDrop へ渡すドラッグ元。
 *
 * 状態を持たないのでスタックに置き、参照カウントは飾りにしてある
 * (DoDragDrop は借りるだけで、返るときには手放している)。
 */
class DropSource final : public IDropSource {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
        if (!object) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDropSource) {
            *object = static_cast<IDropSource*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }   // スタック上なので解放しない
    ULONG STDMETHODCALLTYPE Release() override { return 1; }

    /// Esc で取りやめ、左ボタンを離した時点で落とす
    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escapePressed, DWORD keyState) override {
        if (escapePressed) return DRAGDROP_S_CANCEL;
        if ((keyState & MK_LBUTTON) == 0) return DRAGDROP_S_DROP;
        return S_OK;
    }

    /// カーソルの見た目は OS 既定に任せる(コピー / リンクの区別も含めて)
    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override {
        return DRAGDROP_S_USEDEFAULTCURSORS;
    }
};

/// @brief CF_HDROP の中身(DROPFILES ヘッダ + NUL 区切りのパス列)を作る。
/// @param[in] paths 渡すファイルのパス。
/// @return 確保したメモリ。失敗時は nullptr。
HGLOBAL makeHdrop(const std::vector<std::filesystem::path>& paths) {
    // 各パスを NUL 終端で並べ、末尾にもう 1 つ NUL を置く。落とし先のカレント
    // ディレクトリは当てにできないため必ず絶対パスにする(クリップボードと同じ)
    std::wstring list;
    for (const auto& path : paths) {
        std::error_code ec;
        const std::filesystem::path full = std::filesystem::absolute(path, ec);
        list += (ec ? path : full).native();
        list.push_back(L'\0');
    }
    list.push_back(L'\0');

    const size_t bytes = sizeof(DROPFILES) + list.size() * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GHND, bytes);
    if (!memory) return nullptr;
    void* raw = GlobalLock(memory);
    if (!raw) {
        GlobalFree(memory);
        return nullptr;
    }
    DROPFILES header{};
    header.pFiles = static_cast<DWORD>(sizeof(DROPFILES));  // パス列までのオフセット
    header.fWide = TRUE;
    std::memcpy(raw, &header, sizeof(header));
    std::memcpy(static_cast<uint8_t*>(raw) + sizeof(header), list.data(),
                list.size() * sizeof(wchar_t));
    GlobalUnlock(memory);
    return memory;
}

}  // namespace

void dragFiles(const std::vector<std::filesystem::path>& paths) {
    if (paths.empty()) return;

    // 空のデータオブジェクトはシェルが用意してくれる(IDataObject の自前実装は要らない)
    IDataObject* data = nullptr;
    if (FAILED(SHCreateDataObject(nullptr, 0, nullptr, nullptr, IID_PPV_ARGS(&data)))) return;

    HGLOBAL files = makeHdrop(paths);
    if (!files) {
        data->Release();
        return;
    }
    FORMATETC format{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium{};
    medium.tymed = TYMED_HGLOBAL;
    medium.hGlobal = files;
    // fRelease = TRUE でメモリの所有権をデータオブジェクトへ渡す(成功時は解放不要)
    if (FAILED(data->SetData(&format, &medium, TRUE))) {
        ReleaseStgMedium(&medium);
        data->Release();
        return;
    }

    DropSource source;
    DWORD effect = DROPEFFECT_NONE;
    // 移動は許さない。閲覧中のファイルがドラッグひとつで元の場所から消えるのは危うい
    DoDragDrop(data, &source, DROPEFFECT_COPY | DROPEFFECT_LINK, &effect);
    data->Release();
}

}  // namespace blinker
