#include "audio_devices.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// initguid.h hace que DEFINE_PROPERTYKEY defina de verdad los valores (si no,
// PKEY_Device_FriendlyName queda como un extern sin resolver). propsys.h trae
// el tipo PROPERTYKEY (via wtypes.h) y el macro DEFINE_PROPERTYKEY.
#include <initguid.h>
#include <propsys.h>

#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <propvarutil.h>

// La misma clase del enumerador que usamos en audio_capture.cpp (definida a
// mano para no depender de que MinGW exporte CLSID_MMDeviceEnumerator).
static const CLSID kClsidMMDeviceEnumerator = {
    0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};

static std::string wide_to_utf8(const wchar_t *w) {
    if (!w) return std::string();
    int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return std::string();
    std::string out((size_t)(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &out[0], len, nullptr, nullptr);
    return out;
}

// COM puede ya estar inicializado (por el hilo principal); en ese caso no hay
// que llamar a CoUninitialize al salir.
static bool ensure_com(bool *did_init) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr)) {
        *did_init = true;
        return true;
    }
    return hr == RPC_E_CHANGED_MODE;
}

static std::vector<AudioDeviceInfo> list_devices(EDataFlow flow, const char *def_label) {
    std::vector<AudioDeviceInfo> out;

    bool did_init = false;
    if (!ensure_com(&did_init)) return out;

    IMMDeviceEnumerator *en = nullptr;
    if (SUCCEEDED(CoCreateInstance(kClsidMMDeviceEnumerator, nullptr, CLSCTX_ALL,
                                   __uuidof(IMMDeviceEnumerator), (void **)&en))) {
        out.push_back(AudioDeviceInfo{"", def_label});

        IMMDeviceCollection *coll = nullptr;
        if (SUCCEEDED(en->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &coll))) {
            UINT count = 0;
            coll->GetCount(&count);
            for (UINT i = 0; i < count; i++) {
                IMMDevice *dev = nullptr;
                if (FAILED(coll->Item(i, &dev))) continue;

                std::string id;
                LPWSTR wid = nullptr;
                if (SUCCEEDED(dev->GetId(&wid)) && wid) {
                    id = wide_to_utf8(wid);
                    CoTaskMemFree(wid);
                }

                std::string name;
                IPropertyStore *props = nullptr;
                if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
                    PROPVARIANT pv;
                    PropVariantInit(&pv);
                    if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &pv)))
                        name = wide_to_utf8(pv.vt == VT_LPWSTR ? pv.pwszVal : L"");
                    PropVariantClear(&pv);
                    props->Release();
                }
                if (name.empty()) name = "Dispositivo " + std::to_string(i + 1);

                out.push_back(AudioDeviceInfo{id, name});
                dev->Release();
            }
            coll->Release();
        }
        en->Release();
    }

    if (did_init) CoUninitialize();
    return out;
}

std::vector<AudioDeviceInfo> list_output_devices() {
    return list_devices(eRender, "(Predeterminado)");
}

std::vector<AudioDeviceInfo> list_input_devices() {
    return list_devices(eCapture, "(Predeterminado)");
}
