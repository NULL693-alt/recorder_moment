#include "dxgi_capture.h"

#include <cstdio>

// ---------------------------------------------------------------------------
// Listado de monitores (util cuando el indice pedido no existe)
// ---------------------------------------------------------------------------
void DxgiCapture::list_monitors() {
    IDXGIFactory1 *factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&factory))) return;

    fprintf(stderr, "Monitores disponibles:\n");
    UINT index = 0;
    for (UINT a = 0;; a++) {
        IDXGIAdapter1 *adapter = nullptr;
        if (factory->EnumAdapters1(a, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        for (UINT o = 0;; o++) {
            IDXGIOutput *output = nullptr;
            if (adapter->EnumOutputs(o, &output) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_OUTPUT_DESC desc{};
            output->GetDesc(&desc);
            int w = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
            int h = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;
            char name[128] = "";
            WideCharToMultiByte(CP_UTF8, 0, desc.DeviceName, -1, name, sizeof(name),
                                nullptr, nullptr);
            fprintf(stderr, "  [%u] %s  %dx%d\n", index, name, w, h);
            index++;
            output->Release();
        }
        adapter->Release();
    }
    factory->Release();
}

// ---------------------------------------------------------------------------
// Liberar recursos
// ---------------------------------------------------------------------------
void DxgiCapture::release() {
    if (staging_) { staging_->Release(); staging_ = nullptr; }
    if (duplication_) { duplication_->Release(); duplication_ = nullptr; }
    if (output5_) { output5_->Release(); output5_ = nullptr; }
    if (output1_) { output1_->Release(); output1_ = nullptr; }
    if (output_) { output_->Release(); output_ = nullptr; }
    if (adapter_) { adapter_->Release(); adapter_ = nullptr; }
    if (factory_) { factory_->Release(); factory_ = nullptr; }
    if (device_ctx_) { device_ctx_->Release(); device_ctx_ = nullptr; }
    if (device_) { device_->Release(); device_ = nullptr; }
    mapped_ok_ = false;
}

DxgiCapture::~DxgiCapture() { release(); }

// ---------------------------------------------------------------------------
// Traducir el formato DXGI a nuestro enum
// ---------------------------------------------------------------------------
bool DxgiCapture::classify_format(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_B8G8R8A8_UNORM:
            format_ = CaptureFormat::BGRA;
            return true;
        case DXGI_FORMAT_R8G8B8A8_UNORM:
            format_ = CaptureFormat::RGBA;
            return true;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
            format_ = CaptureFormat::HDR_FLOAT;
            return true;
        default:
            format_ = CaptureFormat::Unknown;
            return false;
    }
}

// ---------------------------------------------------------------------------
// Montar la duplicacion
// ---------------------------------------------------------------------------
HRESULT DxgiCapture::create_duplication() {
    HRESULT hr;

    // Si el monitor soporta IDXGIOutput5 usamos DuplicateOutput1, que acepta
    // una lista de formatos y es la unica via para recibir HDR.
    if (output5_) {
        DXGI_FORMAT formats[] = {
            DXGI_FORMAT_B8G8R8A8_UNORM,           // SDR normal
            DXGI_FORMAT_R16G16B16A16_FLOAT,       // HDR scRGB
        };
        hr = output5_->DuplicateOutput1(device_, 0, 2, formats, &duplication_);
        if (SUCCEEDED(hr)) return hr;
        // Si falla, probamos igual el camino clasico (por si la lista molesta).
    }

    return output1_->DuplicateOutput(device_, &duplication_);
}

// ---------------------------------------------------------------------------
// init
// ---------------------------------------------------------------------------
bool DxgiCapture::init(int monitor_index, bool first_time) {
    release();

    monitor_index_ = monitor_index;

    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&factory_);
    if (FAILED(hr)) {
        fprintf(stderr, "[error] CreateDXGIFactory1 fallo (0x%08lx)\n", (unsigned long)hr);
        return false;
    }

    // Buscamos el adaptador y el output que corresponden al monitor pedido.
    UINT index = 0;
    bool found = false;
    for (UINT a = 0; !found; a++) {
        IDXGIAdapter1 *adapter = nullptr;
        if (factory_->EnumAdapters1(a, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        for (UINT o = 0;; o++) {
            IDXGIOutput *output = nullptr;
            if (adapter->EnumOutputs(o, &output) == DXGI_ERROR_NOT_FOUND) break;
            if ((int)index == monitor_index) {
                adapter_ = adapter;
                output_ = output;
                found = true;
                break;
            }
            output->Release();
            index++;
        }
        if (!found) adapter->Release();
    }

    if (!found) {
        fprintf(stderr, "[error] no existe el monitor #%d\n", monitor_index);
        list_monitors();
        return false;
    }

    // El dispositivo D3D11 tiene que crearse SOBRE ese adaptador.
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    hr = D3D11CreateDevice(adapter_, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, 2,
                           D3D11_SDK_VERSION, &device_, nullptr, &device_ctx_);
    if (FAILED(hr)) {
        fprintf(stderr, "[error] D3D11CreateDevice fallo (0x%08lx)\n", (unsigned long)hr);
        return false;
    }

    hr = output_->QueryInterface(__uuidof(IDXGIOutput1), (void **)&output1_);
    if (FAILED(hr)) {
        fprintf(stderr, "[error] este monitor no soporta captura (IDXGIOutput1) (0x%08lx)\n",
                (unsigned long)hr);
        return false;
    }
    // IDXGIOutput5 es opcional: si esta, habilita HDR.
    output_->QueryInterface(__uuidof(IDXGIOutput5), (void **)&output5_);

    hr = create_duplication();
    if (FAILED(hr)) {
        if (hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE) {
            fprintf(stderr, "[captura] el sistema no permite capturar ahora mismo "
                            "(0x%08lx). Probablemente hay demasiadas capturas activas "
                            "(OBS, Discord, Game Bar). Se reintenta solo...\n",
                    (unsigned long)hr);
        } else {
            fprintf(stderr, "[error] no pude montar la duplicacion (0x%08lx)\n",
                    (unsigned long)hr);
        }
        return false;
    }

    DXGI_OUTDUPL_DESC dup_desc{};
    duplication_->GetDesc(&dup_desc);

    int width = dup_desc.ModeDesc.Width;
    int height = dup_desc.ModeDesc.Height;
    dxgi_format_ = dup_desc.ModeDesc.Format;

    if (!classify_format(dxgi_format_)) {
        fprintf(stderr, "[error] el monitor entrega un formato de pixel no soportado "
                        "(DXGI_FORMAT=%d)\n",
                (int)dxgi_format_);
        return false;
    }

    if (!first_time && (width != width_ || height != height_)) {
        fprintf(stderr, "[error] la resolucion del monitor cambio de %dx%d a %dx%d. No se "
                        "puede cambiar el tamano del video a mitad de una grabacion.\n",
                width_, height_, width, height);
        return false;
    }

    width_ = width;
    height_ = height;

    // Textura "staging": copia del frame en un formato que la CPU si puede
    // leer (D3D11_USAGE_STAGING + CPU_ACCESS_READ).
    D3D11_TEXTURE2D_DESC tex_desc{};
    tex_desc.Width = (UINT)width_;
    tex_desc.Height = (UINT)height_;
    tex_desc.MipLevels = 1;
    tex_desc.ArraySize = 1;
    tex_desc.Format = dxgi_format_;
    tex_desc.SampleDesc.Count = 1;
    tex_desc.Usage = D3D11_USAGE_STAGING;
    tex_desc.BindFlags = 0;
    tex_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    tex_desc.MiscFlags = 0;

    hr = device_->CreateTexture2D(&tex_desc, nullptr, &staging_);
    if (FAILED(hr)) {
        fprintf(stderr, "[error] no pude crear la textura de lectura (0x%08lx)\n",
                (unsigned long)hr);
        return false;
    }

    const char *fmt_name = is_hdr() ? "HDR (se convierte a SDR)" : "SDR";
    fprintf(stderr, "[captura] monitor #%d: %dx%d, formato %s\n", monitor_index_, width_,
            height_, fmt_name);
    return true;
}

// ---------------------------------------------------------------------------
// acquire
// ---------------------------------------------------------------------------
DxgiCapture::Result DxgiCapture::acquire(int timeout_ms) {
    // Desmapeamos el frame anterior (si quedo mapeado).
    if (mapped_ok_) {
        device_ctx_->Unmap(staging_, 0);
        mapped_ok_ = false;
    }

    DXGI_OUTDUPL_FRAME_INFO info{};
    IDXGIResource *resource = nullptr;

    HRESULT hr = duplication_->AcquireNextFrame((UINT)timeout_ms, &info, &resource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return Result::NoChange;
    if (hr == DXGI_ERROR_ACCESS_LOST) return Result::AccessLost;
    if (FAILED(hr)) {
        fprintf(stderr, "[error] AcquireNextFrame fallo (0x%08lx)\n", (unsigned long)hr);
        return Result::Error;
    }

    // Si no hubo un frame nuevo en pantalla, no hay nada que grabar.
    if (info.LastPresentTime.QuadPart == 0) {
        duplication_->ReleaseFrame();
        if (resource) resource->Release();
        return Result::NoChange;
    }

    ID3D11Texture2D *texture = nullptr;
    hr = resource->QueryInterface(__uuidof(ID3D11Texture2D), (void **)&texture);
    resource->Release();
    if (FAILED(hr)) {
        duplication_->ReleaseFrame();
        return Result::NoChange;
    }

    // GPU -> GPU: copia del frame a nuestra textura legible.
    device_ctx_->CopyResource(staging_, texture);
    texture->Release();
    duplication_->ReleaseFrame();

    // Ahora si podemos mapear la textura desde la CPU.
    hr = device_ctx_->Map(staging_, 0, D3D11_MAP_READ, 0, &mapped_);
    if (FAILED(hr)) {
        fprintf(stderr, "[error] no pude mapear la textura (0x%08lx)\n", (unsigned long)hr);
        return Result::Error;
    }
    mapped_ok_ = true;
    return Result::Frame;
}
