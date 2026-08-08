#include "win/winrt_abi.h"

namespace blinker {

const ComBaseApi& comBase() {
    static const ComBaseApi api = [] {
        ComBaseApi a;
        // combase.dll は Windows 8 以降にのみ存在する。無ければ ok = false のまま
        HMODULE module = LoadLibraryExW(L"combase.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) return a;
        const auto load = [module](const char* name) {
            return reinterpret_cast<void*>(GetProcAddress(module, name));
        };
        a.roInitialize = reinterpret_cast<decltype(a.roInitialize)>(load("RoInitialize"));
        a.roUninitialize = reinterpret_cast<decltype(a.roUninitialize)>(load("RoUninitialize"));
        a.roGetActivationFactory =
            reinterpret_cast<decltype(a.roGetActivationFactory)>(load("RoGetActivationFactory"));
        a.windowsCreateString =
            reinterpret_cast<decltype(a.windowsCreateString)>(load("WindowsCreateString"));
        a.windowsDeleteString =
            reinterpret_cast<decltype(a.windowsDeleteString)>(load("WindowsDeleteString"));
        a.windowsGetStringRawBuffer = reinterpret_cast<decltype(a.windowsGetStringRawBuffer)>(
            load("WindowsGetStringRawBuffer"));
        a.ok = a.roInitialize && a.roUninitialize && a.roGetActivationFactory &&
               a.windowsCreateString && a.windowsDeleteString && a.windowsGetStringRawBuffer;
        return a;
    }();
    return api;
}

} // namespace blinker
