// ============================================================================
// video_encoder.h — codificacion de video con FFmpeg
//
// Esta clase encapsula el lado de FFmpeg: elegir el mejor codificador de video
// disponible (h264_qsv en Intel, h264_nvenc en NVIDIA, h264_amf en AMD y
// libx264 como ultimo recurso) y convertir cada frame que llega de la captura
// (BGRA/RGBA/HDR) al formato NV12 que quieren todos los codificadores.
//
// No escribe ningun archivo: cada paquete codificado se entrega por un
// callback. Quien decide donde guardarlo es el ClipBuffer.
// ============================================================================
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
}

class VideoEncoder {
public:
    // Se llama por cada paquete codificado. El AVPacket es valido solo durante
    // la llamada: hay que copiarlo si se quiere conservar.
    using PacketFn = std::function<void(const AVPacket *pkt)>;

    VideoEncoder() = default;
    ~VideoEncoder();

    VideoEncoder(const VideoEncoder &) = delete;
    VideoEncoder &operator=(const VideoEncoder &) = delete;

    // src_fmt es el formato en el que entrega la captura: AV_PIX_FMT_BGRA,
    // AV_PIX_FMT_RGBA o AV_PIX_FMT_RGBAF16 (HDR). target_height, si es mayor
    // que 0 y menor que la altura capturada, reescala el video de salida a esa
    // altura (manteniendo la relacion de aspecto); 0 = resolucion nativa.
    // sdr_white_scale (solo HDR) es el valor scRGB al que Windows pinta el
    // blanco SDR (1.0 = 80 nits); se usa para que el escritorio y los juegos SDR
    // no salgan quemados al pasar a SDR.
    bool open(int width, int height, int fps, int bitrate_kbps, AVPixelFormat src_fmt,
              int target_height = 0, float sdr_white_scale = 1.0f);
    void close();

    // Codifica un frame ya capturado. pts_us es la marca de tiempo en
    // microsegundos. Devuelve false si el codificador fallo.
    bool encode(const uint8_t *data, int pitch, int64_t pts_us, const PacketFn &on_packet);

    // Vacia lo que quede dentro del codificador al terminar.
    void flush(const PacketFn &on_packet);

    AVCodecParameters *codecpar() const { return codecpar_; }
    AVRational time_base() const { return time_base_; }
    const char *name() const { return encoder_name_; }
    bool is_open() const { return codec_ctx_ != nullptr; }
    bool hdr_tonemap() const { return hdr_tonemap_; }
    int out_width() const { return out_width_; }
    int out_height() const { return out_height_; }

private:
    struct Candidate {
        const char *name;
        bool is_qsv;
        const char *preset;
    };

    bool try_open(const Candidate &cand, int fps, int bitrate_kbps);
    // HDR scRGB (float lineal) -> BGRA8 (gamma sRGB) en tonemap_buf_.
    void tonemap_to_bgra(const uint8_t *data, int pitch);
    void drain(const PacketFn &on_packet);

    AVCodecContext *codec_ctx_ = nullptr;
    AVCodecParameters *codecpar_ = nullptr;
    SwsContext *sws_ = nullptr;
    AVFrame *nv12_ = nullptr;
    AVPacket *pkt_ = nullptr;

    AVPixelFormat src_fmt_ = AV_PIX_FMT_BGRA;
    AVRational time_base_{1, 1000000};
    const char *encoder_name_ = nullptr;
    bool hdr_tonemap_ = false;

    int width_ = 0;
    int height_ = 0;
    int out_width_ = 0;  // resolucion del video codificado
    int out_height_ = 0;

    int64_t last_key_us_ = -1; // pts del ultimo keyframe (para forzarlos por tiempo)

    std::vector<uint8_t> tonemap_buf_;
    std::vector<uint8_t> tonemap_lut_; // half-float (los 65536 valores) -> 8 bits sRGB
};
