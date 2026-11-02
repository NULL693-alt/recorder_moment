// ============================================================================
// dxgi_capture.h — captura de pantalla con DXGI Desktop Duplication
//
// Esta clase encapsula TODO el lado de DirectX: elegir el monitor, montar la
// "duplicacion" (Windows nos manda una copia de cada frame que se muestre) y
// dejarnos los pixeles del frame en memoria para que otro los codifique.
//
// Detalles importantes que resuelve:
//
//  * HDR: si el monitor esta en modo HDR el escritorio ya no entrega pixeles
//    BGRA8 sino R16G16B16A16_FLOAT (scRGB). Para eso se usa IDXGIOutput5::
//    DuplicateOutput1, que acepta una LISTA de formatos y nos deja pedir
//    justamente esos. La clase informa el formato real via format()/is_hdr()
//    y el que codifica se encarga de convertir.
//
//  * Coexistencia con OBS / Discord / Xbox Game Bar: en Windows 10/11 se
//    pueden tener varias capturas del mismo monitor a la vez. Cuando el
//    sistema dice "no se puede ahora" (DXGI_ERROR_NOT_CURRENTLY_AVAILABLE) el
//    llamador puede reintentar en vez de abortar; para eso acquire() devuelve
//    Result::AccessLost y init() se puede volver a llamar.
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
#include <dxgi1_2.h>
#include <dxgi1_5.h>

#include <cstdint>
#include <vector>

// Formato de pixel que entrega la captura, ya traducido a algo entendible.
enum class CaptureFormat {
    BGRA,       // 8 bits por canal, SDR (escritorio normal)
    RGBA,       // 8 bits por canal (orden distinto)
    HDR_FLOAT,  // R16G16B16A16_FLOAT: monitor en HDR (scRGB, lineal)
    Unknown,
};

// El cursor del mouse. Desktop Duplication NO lo dibuja en la imagen: avisa
// donde esta y que forma tiene, y lo tenemos que dibujar nosotros.
struct CursorInfo {
    bool visible = false;
    int x = 0, y = 0;            // esquina superior izquierda, en pixeles del monitor
    int width = 0, height = 0;
    // BGRA con alpha normal. Los pixeles que invierten lo que hay debajo (el
    // cursor de texto, por ejemplo) van con alpha = 1 (ver gpu_convert).
    std::vector<uint8_t> bgra;
    int version = 0;             // sube cada vez que cambia la forma
};

class DxgiCapture {
public:
    // Resultado de pedir un frame nuevo.
    enum class Result {
        Frame,       // hay un frame nuevo, visible con data()/row_pitch()
        NoChange,    // todavia no hubo cambios en pantalla
        AccessLost,  // se perdio la duplicacion: hay que volver a iniciar
        Error,       // error fatal
    };

    DxgiCapture() = default;
    ~DxgiCapture();

    DxgiCapture(const DxgiCapture &) = delete;
    DxgiCapture &operator=(const DxgiCapture &) = delete;

    // Abre el monitor 'monitor_index' y monta la duplicacion. Si first_time es
    // false y la resolucion cambio respecto de la anterior, falla: no se puede
    // cambiar el tamano del video a mitad de una grabacion.
    bool init(int monitor_index, bool first_time);
    void release();

    // Pide un frame nuevo, esperando hasta timeout_ms milisegundos.
    Result acquire(int timeout_ms);

    // true si frame_texture() ya tiene algun frame (se puede repetir el ultimo
    // aunque la pantalla no haya cambiado, para mantener los FPS constantes).
    bool has_frame() const { return has_frame_; }

    // Imprime por stderr la lista de monitores detectados.
    static void list_monitors();

    int width() const { return width_; }
    int height() const { return height_; }
    CaptureFormat format() const { return format_; }
    bool is_hdr() const { return format_ == CaptureFormat::HDR_FLOAT; }
    // Solo HDR: valor scRGB del blanco SDR (1.0 = 80 nits), segun el ajuste
    // "brillo del contenido SDR" de Windows. En SDR vale 1.0.
    float sdr_white_scale() const { return sdr_white_scale_; }
    int monitor_index() const { return monitor_index_; }

    // El ultimo frame capturado, en la GPU (se puede leer desde un shader).
    // Cambia en cada init(): generation() sube cada vez, asi quien lo use sabe
    // que tiene que volver a armarse sobre el dispositivo nuevo.
    ID3D11Device *device() const { return device_; }
    ID3D11DeviceContext *context() const { return device_ctx_; }
    ID3D11Texture2D *frame_texture() const { return frame_tex_; }
    int generation() const { return generation_; }

    // Posicion y forma del cursor (se actualizan en cada acquire()).
    const CursorInfo &cursor() const { return cursor_; }

private:
    // Crea la duplicacion probando DuplicateOutput1 (HDR) y, si no esta
    // disponible, DuplicateOutput (SDR clasico).
    HRESULT create_duplication();
    bool classify_format(DXGI_FORMAT f);
    void update_cursor_shape(UINT buffer_size);

    IDXGIFactory1 *factory_ = nullptr;
    IDXGIAdapter1 *adapter_ = nullptr;
    IDXGIOutput *output_ = nullptr;
    IDXGIOutput1 *output1_ = nullptr;
    IDXGIOutput5 *output5_ = nullptr;
    IDXGIOutputDuplication *duplication_ = nullptr;
    ID3D11Device *device_ = nullptr;
    ID3D11DeviceContext *device_ctx_ = nullptr;
    ID3D11Texture2D *frame_tex_ = nullptr;
    bool has_frame_ = false; // frame_tex_ ya tiene un frame copiado
    int generation_ = 0;
    CursorInfo cursor_;
    std::vector<uint8_t> shape_buf_;

    DXGI_FORMAT dxgi_format_ = DXGI_FORMAT_B8G8R8A8_UNORM;
    CaptureFormat format_ = CaptureFormat::BGRA;
    int width_ = 0;
    int height_ = 0;
    int monitor_index_ = 0;
    float sdr_white_scale_ = 1.0f;
};
