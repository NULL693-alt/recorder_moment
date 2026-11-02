#include "gpu_convert.h"

#include <d3dcompiler.h>

#include <cstdio>
#include <cstring>

// ---------------------------------------------------------------------------
// Shaders
//
// Un triangulo que tapa toda la pantalla (sin vertex buffer) y dos pixel
// shaders: uno escribe Y y el otro UV. Cada pixel de salida lee la textura
// capturada con filtro bilineal; si se esta achicando, promedia 4 lecturas
// para no perder detalle de golpe (aliasing).
// ---------------------------------------------------------------------------
static const char kShaderSource[] = R"(
Texture2D src : register(t0);
Texture2D cur : register(t1); // forma del cursor (BGRA)
SamplerState smp : register(s0);

cbuffer Params : register(b0) {
    float2 out_size;    // tamano del destino de esta pasada
    float2 tap;         // separacion de las 4 lecturas (en uv); 0 = una sola
    float white;        // HDR: valor scRGB del blanco SDR; 0 = la entrada es SDR
    float cursor_on;    // 1 = dibujar el cursor
    float2 in_size;     // tamano de la captura
    float4 cursor_rect; // x, y, ancho, alto del cursor (pixeles de la captura)
};

void vs_main(uint id : SV_VertexID, out float4 pos : SV_Position) {
    float2 t = float2((id << 1) & 2, id & 2);
    pos = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
}

// HDR (scRGB lineal) -> SDR con curva sRGB. Lo que en el monitor se ve como
// contenido SDR queda igual; lo que pasa de un 75% se comprime de a poco.
float3 tonemap(float3 c) {
    float3 x = max(c / white, 0);
    const float k = 0.75;
    float3 shoulder = k + (1 - k) * (1 - exp(-(x - k) / (1 - k)));
    x = lerp(shoulder, x, step(x, k));
    float3 lo = x * 12.92;
    float3 hi = 1.055 * pow(x, 1.0 / 2.4) - 0.055;
    return saturate(lerp(hi, lo, step(x, 0.0031308)));
}

// El cursor se dibuja encima (ya en SDR). Alpha = 1/255 marca los pixeles que
// invierten el fondo (cursor de texto).
float3 with_cursor(float3 c, float2 uv) {
    if (cursor_on <= 0) return c;
    float2 p = uv * in_size - cursor_rect.xy;
    if (p.x < 0 || p.y < 0 || p.x >= cursor_rect.z || p.y >= cursor_rect.w) return c;
    float4 k = cur.Load(int3(int2(p), 0));
    if (k.a > 0.5 / 255 && k.a < 1.5 / 255) return 1 - c;
    return lerp(c, k.rgb, k.a);
}

float3 fetch(float2 uv) {
    float3 c;
    if (tap.x > 0) {
        c = 0.25 * (src.Sample(smp, uv + float2(-tap.x, -tap.y)).rgb +
                    src.Sample(smp, uv + float2( tap.x, -tap.y)).rgb +
                    src.Sample(smp, uv + float2(-tap.x,  tap.y)).rgb +
                    src.Sample(smp, uv + float2( tap.x,  tap.y)).rgb);
    } else {
        c = src.Sample(smp, uv).rgb;
    }
    return with_cursor(white > 0 ? tonemap(c) : c, uv);
}

static const float3 kLuma = float3(0.2126, 0.7152, 0.0722); // BT.709

float ps_y(float4 pos : SV_Position) : SV_Target {
    float3 c = fetch(pos.xy / out_size);
    return dot(c, kLuma) * (219.0 / 255.0) + 16.0 / 255.0;
}

float2 ps_uv(float4 pos : SV_Position) : SV_Target {
    // En la pasada UV cada pixel cubre 2x2 pixeles de Y: leer justo en la
    // esquina entre los 4 con filtro bilineal ya los promedia.
    float3 c = fetch(pos.xy / out_size);
    float y = dot(c, kLuma);
    float u = (c.b - y) / 1.8556;
    float v = (c.r - y) / 1.5748;
    return float2(u, v) * (224.0 / 255.0) + 128.0 / 255.0;
}
)";

template <typename T> static void release(T *&p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

static ID3DBlob *compile(const char *entry, const char *target) {
    ID3DBlob *code = nullptr;
    ID3DBlob *errors = nullptr;
    HRESULT hr = D3DCompile(kShaderSource, sizeof(kShaderSource) - 1, "gpu_convert", nullptr,
                            nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code,
                            &errors);
    if (FAILED(hr)) {
        fprintf(stderr, "[gpu] no compilo el shader %s (0x%08lx): %s\n", entry,
                (unsigned long)hr,
                errors ? (const char *)errors->GetBufferPointer() : "sin detalle");
        release(code);
    }
    release(errors);
    return code;
}

static ID3D11Texture2D *make_texture(ID3D11Device *dev, int w, int h, DXGI_FORMAT fmt,
                                     bool readable) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = (UINT)w;
    d.Height = (UINT)h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = fmt;
    d.SampleDesc.Count = 1;
    d.Usage = readable ? D3D11_USAGE_STAGING : D3D11_USAGE_DEFAULT;
    d.BindFlags = readable ? 0 : D3D11_BIND_RENDER_TARGET;
    d.CPUAccessFlags = readable ? D3D11_CPU_ACCESS_READ : 0;
    ID3D11Texture2D *tex = nullptr;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &tex))) return nullptr;
    return tex;
}

// Los parametros cambian en cada frame (la posicion del cursor): buffer dinamico.
static ID3D11Buffer *make_params(ID3D11Device *dev, UINT size) {
    D3D11_BUFFER_DESC d{};
    d.ByteWidth = size;
    d.Usage = D3D11_USAGE_DYNAMIC;
    d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Buffer *buf = nullptr;
    if (FAILED(dev->CreateBuffer(&d, nullptr, &buf))) return nullptr;
    return buf;
}

static bool write_params(ID3D11DeviceContext *ctx, ID3D11Buffer *buf, const void *data,
                         size_t size) {
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(buf, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
    memcpy(m.pData, data, size);
    ctx->Unmap(buf, 0);
    return true;
}

GpuConverter::~GpuConverter() { close(); }

void GpuConverter::close() {
    for (int i = 0; i < kDepth; i++) {
        release(y_read_[i]);
        release(uv_read_[i]);
    }
    release(y_rtv_);
    release(uv_rtv_);
    release(y_tex_);
    release(uv_tex_);
    release(cb_y_);
    release(cb_uv_);
    release(cursor_srv_);
    release(cursor_tex_);
    cursor_w_ = cursor_h_ = 0;
    cursor_visible_ = false;
    release(sampler_);
    release(ps_y_);
    release(ps_uv_);
    release(vs_);
    release(src_srv_);
    release(ctx_);
    release(device_);
    submitted_ = 0;
}

// ---------------------------------------------------------------------------
// open
// ---------------------------------------------------------------------------
bool GpuConverter::open(ID3D11Device *device, ID3D11DeviceContext *ctx, ID3D11Texture2D *src,
                        int in_w, int in_h, int out_w, int out_h, float hdr_white) {
    close();
    if (!device || !ctx || !src) return false;
    device_ = device;
    device_->AddRef();
    ctx_ = ctx;
    ctx_->AddRef();
    out_w_ = out_w;
    out_h_ = out_h;

    bool ok = true;
    ok = ok && SUCCEEDED(device_->CreateShaderResourceView(src, nullptr, &src_srv_));

    ID3DBlob *vs = ok ? compile("vs_main", "vs_5_0") : nullptr;
    ID3DBlob *psy = ok ? compile("ps_y", "ps_5_0") : nullptr;
    ID3DBlob *psuv = ok ? compile("ps_uv", "ps_5_0") : nullptr;
    ok = ok && vs && psy && psuv;
    ok = ok && SUCCEEDED(device_->CreateVertexShader(vs->GetBufferPointer(),
                                                     vs->GetBufferSize(), nullptr, &vs_));
    ok = ok && SUCCEEDED(device_->CreatePixelShader(psy->GetBufferPointer(),
                                                    psy->GetBufferSize(), nullptr, &ps_y_));
    ok = ok && SUCCEEDED(device_->CreatePixelShader(psuv->GetBufferPointer(),
                                                    psuv->GetBufferSize(), nullptr, &ps_uv_));
    release(vs);
    release(psy);
    release(psuv);

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    ok = ok && SUCCEEDED(device_->CreateSamplerState(&sd, &sampler_));

    // Si se achica la imagen, cada pixel de salida promedia 4 lecturas.
    bool scaled = (in_w != out_w || in_h != out_h);
    py_ = Params{(float)out_w, (float)out_h, 0, 0, hdr_white, 0,
                 (float)in_w, (float)in_h, 0, 0, 0, 0};
    puv_ = py_;
    puv_.out_w = (float)(out_w / 2);
    puv_.out_h = (float)(out_h / 2);
    if (scaled) {
        py_.tap_x = 0.25f / (float)out_w;
        py_.tap_y = 0.25f / (float)out_h;
        puv_.tap_x = 0.25f / (float)(out_w / 2);
        puv_.tap_y = 0.25f / (float)(out_h / 2);
    }
    if (ok) {
        cb_y_ = make_params(device_, sizeof(Params));
        cb_uv_ = make_params(device_, sizeof(Params));
        ok = cb_y_ && cb_uv_;
    }

    if (ok) {
        y_tex_ = make_texture(device_, out_w, out_h, DXGI_FORMAT_R8_UNORM, false);
        uv_tex_ = make_texture(device_, out_w / 2, out_h / 2, DXGI_FORMAT_R8G8_UNORM, false);
        ok = y_tex_ && uv_tex_;
        ok = ok && SUCCEEDED(device_->CreateRenderTargetView(y_tex_, nullptr, &y_rtv_));
        ok = ok && SUCCEEDED(device_->CreateRenderTargetView(uv_tex_, nullptr, &uv_rtv_));
    }
    for (int i = 0; ok && i < kDepth; i++) {
        y_read_[i] = make_texture(device_, out_w, out_h, DXGI_FORMAT_R8_UNORM, true);
        uv_read_[i] = make_texture(device_, out_w / 2, out_h / 2, DXGI_FORMAT_R8G8_UNORM, true);
        ok = y_read_[i] && uv_read_[i];
    }

    if (!ok) {
        fprintf(stderr, "[gpu] no pude preparar la conversion de color en la GPU\n");
        close();
        return false;
    }
    fprintf(stderr, "[gpu] conversion a NV12 en la GPU: %dx%d -> %dx%d%s\n", in_w, in_h, out_w,
            out_h, hdr_white > 0 ? " (HDR -> SDR)" : "");
    if (hdr_white > 0)
        fprintf(stderr, "[gpu] HDR: blanco SDR = %.0f nits\n", hdr_white * 80.0f);
    return true;
}

// ---------------------------------------------------------------------------
// Una pasada: dibuja el triangulo de pantalla completa sobre rtv.
// ---------------------------------------------------------------------------
bool GpuConverter::draw(ID3D11RenderTargetView *rtv, ID3D11PixelShader *ps, ID3D11Buffer *cb,
                        int w, int h) {
    D3D11_VIEWPORT vp{};
    vp.Width = (float)w;
    vp.Height = (float)h;
    vp.MaxDepth = 1.0f;

    ctx_->OMSetRenderTargets(1, &rtv, nullptr);
    ctx_->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    ctx_->OMSetDepthStencilState(nullptr, 0);
    ctx_->RSSetState(nullptr);
    ctx_->RSSetViewports(1, &vp);
    ctx_->IASetInputLayout(nullptr);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->VSSetShader(vs_, nullptr, 0);
    ctx_->PSSetShader(ps, nullptr, 0);
    ctx_->PSSetConstantBuffers(0, 1, &cb);
    ID3D11ShaderResourceView *srvs[2] = {src_srv_, cursor_srv_};
    ctx_->PSSetShaderResources(0, 2, srvs);
    ctx_->PSSetSamplers(0, 1, &sampler_);
    ctx_->Draw(3, 0);
    return true;
}

// ---------------------------------------------------------------------------
// Leer una de las copias desde la CPU y entregarla.
// ---------------------------------------------------------------------------
bool GpuConverter::read_slot(int slot, const ReadyFn &on_ready) {
    D3D11_MAPPED_SUBRESOURCE my{}, muv{};
    if (FAILED(ctx_->Map(y_read_[slot], 0, D3D11_MAP_READ, 0, &my))) {
        fprintf(stderr, "[gpu] no pude leer el plano Y\n");
        return false;
    }
    if (FAILED(ctx_->Map(uv_read_[slot], 0, D3D11_MAP_READ, 0, &muv))) {
        ctx_->Unmap(y_read_[slot], 0);
        fprintf(stderr, "[gpu] no pude leer el plano UV\n");
        return false;
    }
    bool ok = on_ready((const uint8_t *)my.pData, (int)my.RowPitch, (const uint8_t *)muv.pData,
                       (int)muv.RowPitch, pts_[slot]);
    ctx_->Unmap(uv_read_[slot], 0);
    ctx_->Unmap(y_read_[slot], 0);
    return ok;
}

// ---------------------------------------------------------------------------
// Cursor
// ---------------------------------------------------------------------------
bool GpuConverter::set_cursor_shape(const uint8_t *bgra, int w, int h) {
    release(cursor_srv_);
    release(cursor_tex_);
    cursor_w_ = cursor_h_ = 0;
    if (!device_ || !bgra || w <= 0 || h <= 0) return false;

    D3D11_TEXTURE2D_DESC d{};
    d.Width = (UINT)w;
    d.Height = (UINT)h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_IMMUTABLE;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem = bgra;
    init.SysMemPitch = (UINT)w * 4;
    if (FAILED(device_->CreateTexture2D(&d, &init, &cursor_tex_)) ||
        FAILED(device_->CreateShaderResourceView(cursor_tex_, nullptr, &cursor_srv_))) {
        release(cursor_tex_);
        return false;
    }
    cursor_w_ = w;
    cursor_h_ = h;
    return true;
}

void GpuConverter::set_cursor_pos(int x, int y, bool visible) {
    cursor_x_ = x;
    cursor_y_ = y;
    cursor_visible_ = visible;
}

// ---------------------------------------------------------------------------
// submit
// ---------------------------------------------------------------------------
bool GpuConverter::submit(int64_t pts_us, const ReadyFn &on_ready) {
    if (!device_) return false;

    // Posicion del cursor de este frame.
    bool cursor_on = cursor_visible_ && cursor_srv_;
    for (Params *p : {&py_, &puv_}) {
        p->cursor_on = cursor_on ? 1.0f : 0.0f;
        p->cur_x = (float)cursor_x_;
        p->cur_y = (float)cursor_y_;
        p->cur_w = (float)cursor_w_;
        p->cur_h = (float)cursor_h_;
    }
    if (!write_params(ctx_, cb_y_, &py_, sizeof(py_)) ||
        !write_params(ctx_, cb_uv_, &puv_, sizeof(puv_)))
        return false;

    int slot = (int)(submitted_ % kDepth);
    draw(y_rtv_, ps_y_, cb_y_, out_w_, out_h_);
    draw(uv_rtv_, ps_uv_, cb_uv_, out_w_ / 2, out_h_ / 2);

    // Soltamos la textura capturada y los destinos: la captura vuelve a
    // escribir en ella y las copias de abajo los leen.
    ID3D11ShaderResourceView *no_srv[2] = {nullptr, nullptr};
    ctx_->PSSetShaderResources(0, 2, no_srv);
    ctx_->OMSetRenderTargets(0, nullptr, nullptr);

    ctx_->CopyResource(y_read_[slot], y_tex_);
    ctx_->CopyResource(uv_read_[slot], uv_tex_);
    pts_[slot] = pts_us;
    submitted_++;

    // El frame de hace kDepth - 1 llamadas ya lo termino la GPU: leerlo no
    // la frena.
    if (submitted_ < kDepth) return true;
    int ready = (int)((submitted_ - kDepth) % kDepth);
    return read_slot(ready, on_ready);
}
