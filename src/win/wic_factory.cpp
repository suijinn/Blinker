#include "win/wic_factory.h"

#include <windows.h>

#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>

namespace blinker {

IWICImagingFactory* wicFactoryForThisThread() {
    thread_local struct ThreadState {
        bool comInitialized = false;
        Microsoft::WRL::ComPtr<IWICImagingFactory> factory;

        ThreadState() {
            // RPC_E_CHANGED_MODE(既に別モードで初期化済み)でも COM は利用できる
            comInitialized = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
            CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                             IID_PPV_ARGS(&factory));
        }
        ~ThreadState() {
            factory.Reset();
            if (comInitialized) CoUninitialize();
        }
    } state;
    return state.factory.Get();
}

} // namespace blinker
