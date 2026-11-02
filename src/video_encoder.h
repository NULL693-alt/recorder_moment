// ============================================================================
// video_encoder.h — codificacion de video con FFmpeg
//
// Esta clase encapsula el lado de FFmpeg: elegir el mejor codificador de video
// disponible (h264_qsv en Intel, h264_nvenc en NVIDIA, h264_amf en AMD y
// libx264 como ultimo recurso) y codificar cada frame NV12 que le llega. La
// conversion de color (BGRA/RGBA/HDR -> NV12) y el escalado ya vienen hechos
// desde la GPU (gpu_convert.*).
//
// No escribe ningun archivo: cada paquete codificado se entrega por un
// callback. Quien decide donde guardarlo es el ClipBuffer.
// ============================================================================
#pragma once

#include <cstdint>
#include <functional>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
}

// Codec de video de los clips. AV1 pesa bastante menos a la misma calidad,
// pero necesita una GPU que lo codifique (Intel Arc, NVIDIA RTX 40+, AMD RX
// 7000+) y no todos los programas lo reproducen; H.264 anda en todos lados.
enum class VideoCodec { H264, AV1 };
const char *video_codec_name(VideoCodec c);

class VideoEncoder {
public:
    // Se llama por cada paquete codificado. El AVPacket es valido solo durante
    // la llamada: hay que copiarlo si se quiere conservar.
    using PacketFn = std::function<void(const AVPacket *pkt)>;

    VideoEncoder() = default;
    ~VideoEncoder();

    VideoEncoder(const VideoEncoder &) = delete;
    VideoEncoder &operator=(const VideoEncoder &) = delete;

    // width/height es lo que entrega la captura. target_height, si es mayor
    // que 0 y menor que la altura capturada, baja el video de salida a esa
    // altura (manteniendo la relacion de aspecto); 0 = resolucion nativa. La
    // resolucion final queda en out_width()/out_height(): los frames NV12 que
    // se le pasen tienen que venir de ese tamano. Si se pide AV1 y la GPU no
    // lo soporta, se usa H.264 (codec() dice cual quedo).
    bool open(int width, int height, int fps, int bitrate_kbps, int target_height = 0,
              VideoCodec codec = VideoCodec::H264);
    void close();

    // Codifica un frame NV12 (plano Y + plano UV intercalado) del tamano de
    // salida. pts_us es la marca de tiempo en microsegundos. Devuelve false si
    // el codificador fallo.
    bool encode_nv12(const uint8_t *y, int y_pitch, const uint8_t *uv, int uv_pitch,
                     int64_t pts_us, const PacketFn &on_packet);

    // Vacia lo que quede dentro del codificador al terminar.
    void flush(const PacketFn &on_packet);

    AVCodecParameters *codecpar() const { return codecpar_; }
    AVRational time_base() const { return time_base_; }
    const char *name() const { return encoder_name_; }
    VideoCodec codec() const { return codec_; }
    bool is_open() const { return codec_ctx_ != nullptr; }
    int out_width() const { return out_width_; }
    int out_height() const { return out_height_; }

private:
    struct Candidate {
        const char *name;
        bool is_qsv;
        const char *preset;
    };

    bool try_open(const Candidate &cand, int fps, int bitrate_kbps);
    void drain(const PacketFn &on_packet);

    AVCodecContext *codec_ctx_ = nullptr;
    AVCodecParameters *codecpar_ = nullptr;
    AVFrame *nv12_ = nullptr;
    AVPacket *pkt_ = nullptr;

    AVRational time_base_{1, 1000000};
    const char *encoder_name_ = nullptr;
    VideoCodec codec_ = VideoCodec::H264;

    int out_width_ = 0;  // resolucion del video codificado
    int out_height_ = 0;

    int64_t last_key_us_ = -1; // pts del ultimo keyframe (para forzarlos por tiempo)
};
