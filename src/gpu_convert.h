// ============================================================================
// gpu_convert.h — conversion de color en la GPU (RGB/HDR -> NV12)
//
// Antes cada frame bajaba entero de la GPU a la CPU (4 u 8 bytes por pixel) y
// la CPU lo convertia a NV12 (y hacia el tonemap HDR). Ahora todo eso lo hacen
// dos shaders sobre la textura capturada:
//
//   * pasada Y : luminancia, a la resolucion de salida (R8)
//   * pasada UV: color, a la mitad de resolucion (R8G8)
//
// en el mismo paso se escala (si se pidio una resolucion menor) y, si el
// monitor esta en HDR, se pasa a SDR. A la CPU solo baja el NV12 ya listo
// (1,5 bytes por pixel), y para no frenar a la GPU esperando cada copia se usan
// varias texturas de lectura que se turnan: el frame que se lee es el de hace
// kDepth - 1 llamadas, que la GPU ya termino hace rato.
//
// El color sale en BT.709 rango limitado (lo estandar para video HD).
// ============================================================================
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>

#include <cstdint>
#include <functional>

class GpuConverter {
public:
    // Se llama con cada frame NV12 listo (planos Y y UV en memoria de la CPU,
    // validos solo durante la llamada). Devuelve false si algo fallo.
    using ReadyFn = std::function<bool(const uint8_t *y, int y_pitch, const uint8_t *uv,
                                       int uv_pitch, int64_t pts_us)>;

    GpuConverter() = default;
    ~GpuConverter();

    GpuConverter(const GpuConverter &) = delete;
    GpuConverter &operator=(const GpuConverter &) = delete;

    // src es la textura capturada (in_w x in_h). out_w/out_h, la resolucion de
    // salida (pares). hdr_white > 0 indica que src es scRGB (HDR) y es el valor
    // del blanco SDR; 0 = src es SDR (BGRA/RGBA 8 bits).
    bool open(ID3D11Device *device, ID3D11DeviceContext *ctx, ID3D11Texture2D *src, int in_w,
              int in_h, int out_w, int out_h, float hdr_white);
    void close();
    bool is_open() const { return device_ != nullptr; }

    // Convierte lo que hay ahora en src. Si con esto sale un frame de la cola
    // (el de hace kDepth - 1 llamadas), se entrega a on_ready.
    bool submit(int64_t pts_us, const ReadyFn &on_ready);

    // Cursor del mouse, dibujado encima de la imagen en la misma pasada. La
    // forma es BGRA (w x h); alpha = 1 marca pixeles que invierten el fondo.
    // x/y es la esquina superior izquierda en pixeles de la captura.
    bool set_cursor_shape(const uint8_t *bgra, int w, int h);
    void set_cursor_pos(int x, int y, bool visible);

private:
    static const int kDepth = 3; // texturas de lectura que se turnan

    // Igual que el cbuffer Params del shader (en bloques de 16 bytes).
    struct Params {
        float out_w, out_h;
        float tap_x, tap_y;
        float white;
        float cursor_on;
        float in_w, in_h;
        float cur_x, cur_y, cur_w, cur_h;
    };

    bool draw(ID3D11RenderTargetView *rtv, ID3D11PixelShader *ps, ID3D11Buffer *cb, int w,
              int h);
    bool read_slot(int slot, const ReadyFn &on_ready);

    ID3D11Device *device_ = nullptr;
    ID3D11DeviceContext *ctx_ = nullptr;
    ID3D11ShaderResourceView *src_srv_ = nullptr;
    ID3D11VertexShader *vs_ = nullptr;
    ID3D11PixelShader *ps_y_ = nullptr;
    ID3D11PixelShader *ps_uv_ = nullptr;
    ID3D11SamplerState *sampler_ = nullptr;
    ID3D11Buffer *cb_y_ = nullptr;
    ID3D11Buffer *cb_uv_ = nullptr;

    ID3D11Texture2D *y_tex_ = nullptr;   // destino de la pasada Y
    ID3D11Texture2D *uv_tex_ = nullptr;  // destino de la pasada UV
    ID3D11RenderTargetView *y_rtv_ = nullptr;
    ID3D11RenderTargetView *uv_rtv_ = nullptr;

    ID3D11Texture2D *y_read_[kDepth] = {};  // copias que la CPU puede leer
    ID3D11Texture2D *uv_read_[kDepth] = {};
    int64_t pts_[kDepth] = {};

    ID3D11Texture2D *cursor_tex_ = nullptr;
    ID3D11ShaderResourceView *cursor_srv_ = nullptr;
    int cursor_x_ = 0, cursor_y_ = 0, cursor_w_ = 0, cursor_h_ = 0;
    bool cursor_visible_ = false;

    // Parametros de cada pasada (se reescriben en cada frame).
    Params py_{};
    Params puv_{};

    int out_w_ = 0;
    int out_h_ = 0;
    int64_t submitted_ = 0; // frames enviados desde open()
};
