#include "dxgi_capture.h"

#include <cstdio>
#include <cwchar>
#include <vector>

// ---------------------------------------------------------------------------
// HDR: a que brillo pinta Windows el blanco SDR en este monitor (el ajuste
// "brillo del contenido SDR"). Devuelve el valor scRGB (1.0 = 80 nits), o 0 si
// no se pudo averiguar. gdi_name es el DeviceName del output (\\.\DISPLAY1).
// ---------------------------------------------------------------------------
static float query_sdr_white_scale(const wchar_t *gdi_name) {
    UINT32 npaths = 0, nmodes = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &npaths, &nmodes) != ERROR_SUCCESS)
        return 0.0f;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(npaths);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(nmodes);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &npaths, paths.data(), &nmodes, modes.data(),
                           nullptr) != ERROR_SUCCESS)
        return 0.0f;

    for (UINT32 i = 0; i < npaths; i++) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME src{};
        src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        src.header.size = sizeof(src);
        src.header.adapterId = paths[i].sourceInfo.adapterId;
        src.header.id = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS) continue;
        if (wcscmp(src.viewGdiDeviceName, gdi_name) != 0) continue;

        DISPLAYCONFIG_SDR_WHITE_LEVEL white{};
        white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
        white.header.size = sizeof(white);
        white.header.adapterId = paths[i].targetInfo.adapterId;
        white.header.id = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&white.header) != ERROR_SUCCESS) return 0.0f;
        // SDRWhiteLevel viene en milesimas: 1000 = 80 nits = 1.0 en scRGB.
        return (float)white.SDRWhiteLevel / 1000.0f;
    }
    return 0.0f;
}

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
    if (frame_tex_) { frame_tex_->Release(); frame_tex_ = nullptr; }
    if (duplication_) { duplication_->Release(); duplication_ = nullptr; }
    if (output5_) { output5_->Release(); output5_ = nullptr; }
    if (output1_) { output1_->Release(); output1_ = nullptr; }
    if (output_) { output_->Release(); output_ = nullptr; }
    if (adapter_) { adapter_->Release(); adapter_ = nullptr; }
    if (factory_) { factory_->Release(); factory_ = nullptr; }
    if (device_ctx_) { device_ctx_->Release(); device_ctx_ = nullptr; }
    if (device_) { device_->Release(); device_ = nullptr; }
    has_frame_ = false;
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

    sdr_white_scale_ = 1.0f;
    if (is_hdr()) {
        DXGI_OUTPUT_DESC out_desc{};
        output_->GetDesc(&out_desc);
        float s = query_sdr_white_scale(out_desc.DeviceName);
        // Si Windows no lo informa, 200 nits es un valor tipico.
        sdr_white_scale_ = s >= 1.0f ? s : 2.5f;
    }

    // Textura donde queda el ultimo frame, en la GPU. Se puede leer desde un
    // shader (la conversion a NV12 se hace ahi, ver gpu_convert.*).
    D3D11_TEXTURE2D_DESC tex_desc{};
    tex_desc.Width = (UINT)width_;
    tex_desc.Height = (UINT)height_;
    tex_desc.MipLevels = 1;
    tex_desc.ArraySize = 1;
    tex_desc.Format = dxgi_format_;
    tex_desc.SampleDesc.Count = 1;
    tex_desc.Usage = D3D11_USAGE_DEFAULT;
    tex_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    tex_desc.CPUAccessFlags = 0;
    tex_desc.MiscFlags = 0;

    hr = device_->CreateTexture2D(&tex_desc, nullptr, &frame_tex_);
    if (FAILED(hr)) {
        fprintf(stderr, "[error] no pude crear la textura del frame (0x%08lx)\n",
                (unsigned long)hr);
        return false;
    }
    generation_++;
    cursor_.version++; // quien dibuja el cursor lo vuelve a subir al dispositivo nuevo

    const char *fmt_name = is_hdr() ? "HDR (se convierte a SDR)" : "SDR";
    fprintf(stderr, "[captura] monitor #%d: %dx%d, formato %s\n", monitor_index_, width_,
            height_, fmt_name);
    return true;
}

// ---------------------------------------------------------------------------
// acquire
// ---------------------------------------------------------------------------
DxgiCapture::Result DxgiCapture::acquire(int timeout_ms) {
    DXGI_OUTDUPL_FRAME_INFO info{};
    IDXGIResource *resource = nullptr;

    HRESULT hr = duplication_->AcquireNextFrame((UINT)timeout_ms, &info, &resource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return Result::NoChange;
    if (hr == DXGI_ERROR_ACCESS_LOST) return Result::AccessLost;
    if (FAILED(hr)) {
        fprintf(stderr, "[error] AcquireNextFrame fallo (0x%08lx)\n", (unsigned long)hr);
        return Result::Error;
    }

    // Cursor: si se movio, nueva posicion; si cambio de forma, la nueva forma.
    // (Esto llega aunque la pantalla no haya cambiado.)
    if (info.LastMouseUpdateTime.QuadPart != 0) {
        cursor_.visible = info.PointerPosition.Visible != FALSE;
        cursor_.x = info.PointerPosition.Position.x;
        cursor_.y = info.PointerPosition.Position.y;
    }
    if (info.PointerShapeBufferSize > 0) update_cursor_shape(info.PointerShapeBufferSize);

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

    // GPU -> GPU: copia del frame a nuestra textura (no pasa por la CPU).
    device_ctx_->CopyResource(frame_tex_, texture);
    texture->Release();
    duplication_->ReleaseFrame();

    has_frame_ = true;
    return Result::Frame;
}

// ---------------------------------------------------------------------------
// Forma del cursor -> BGRA. Windows la manda en tres formatos:
//   * COLOR: BGRA con transparencia normal.
//   * MASKED_COLOR: BGRA donde alpha 0 = pintar ese color y alpha 255 = hacer
//     XOR con lo que hay debajo.
//   * MONOCHROME: dos mascaras de 1 bit (AND y XOR), una debajo de la otra.
// Los pixeles que "invierten" el fondo (cursor de texto) no se pueden
// representar con alpha normal: los marcamos con alpha = 1 y el shader los
// dibuja invirtiendo el color de abajo.
// ---------------------------------------------------------------------------
void DxgiCapture::update_cursor_shape(UINT buffer_size) {
    if (shape_buf_.size() < buffer_size) shape_buf_.resize(buffer_size);
    UINT required = 0;
    DXGI_OUTDUPL_POINTER_SHAPE_INFO si{};
    if (FAILED(duplication_->GetFramePointerShape(buffer_size, shape_buf_.data(), &required,
                                                  &si)))
        return;

    const bool mono = si.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME;
    const int w = (int)si.Width;
    const int h = mono ? (int)si.Height / 2 : (int)si.Height;
    const uint8_t *src = shape_buf_.data();
    std::vector<uint8_t> out((size_t)w * h * 4, 0);

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint8_t *d = &out[((size_t)y * w + x) * 4];
            if (mono) {
                int bit = 7 - (x & 7);
                int and_bit = (src[(size_t)y * si.Pitch + x / 8] >> bit) & 1;
                int xor_bit = (src[(size_t)(y + h) * si.Pitch + x / 8] >> bit) & 1;
                if (!and_bit) { // pintar negro o blanco
                    uint8_t v = xor_bit ? 255 : 0;
                    d[0] = d[1] = d[2] = v;
                    d[3] = 255;
                } else if (xor_bit) {
                    d[3] = 1; // invertir
                }
                continue;
            }
            const uint8_t *s = src + (size_t)y * si.Pitch + (size_t)x * 4;
            if (si.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR) {
                if (s[3] == 0) {
                    d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
                    d[3] = 255;
                } else if (s[0] | s[1] | s[2]) {
                    d[3] = 1; // XOR con un color: lo aproximamos invirtiendo
                }
            } else { // COLOR
                d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
                d[3] = s[3] < 3 ? 0 : s[3]; // alpha 1 y 2 quedan reservados
            }
        }
    }

    cursor_.width = w;
    cursor_.height = h;
    cursor_.bgra.swap(out);
    cursor_.version++;
}
