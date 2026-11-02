// ============================================================================
// clip_buffer.h — buffer circular de video codificado (estilo Medal)
//
// Mientras el programa corre, cada frame se codifica y los paquetes (H.264 ya
// comprimido) se guardan en una cola en memoria. Lo mismo se hace con el audio
// (AAC), en una cola aparte. Las colas se podan solas para no crecer mas alla
// de unos segundos mas que la ventana pedida.
//
// Cuando el usuario aprieta el atajo (F9) se llama a save_to(): se crea un
// .mp4 nuevo y se escriben los ultimos N segundos de video y audio, arrancando
// el video desde un "keyframe" para que el clip sea reproducible desde el
// inicio.
// ============================================================================
#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/rational.h>
}

class ClipBuffer {
public:
    // seconds: cuantos segundos de video se conservan.
    explicit ClipBuffer(int seconds);
    ~ClipBuffer();

    ClipBuffer(const ClipBuffer &) = delete;
    ClipBuffer &operator=(const ClipBuffer &) = delete;

    // Guarda una copia de un paquete de video. pts_us es su marca de tiempo
    // (para saber de que antiguedad es) y keyframe indica si es un frame clave.
    void push_video(const AVPacket *pkt, int64_t pts_us, bool keyframe);

    // Igual para un paquete de audio. pts_us es su marca de tiempo.
    void push_audio(const AVPacket *pkt, int64_t pts_us);

    void clear();

    // Copia de lo que hay ahora en el buffer. Los datos de los paquetes se
    // comparten (conteo de referencias de FFmpeg), asi que es barata: sirve
    // para escribir el clip en otro hilo sin frenar la captura.
    std::unique_ptr<ClipBuffer> snapshot() const;

    // Escribe en 'path' los ultimos 'seconds' segundos. Si audio_par es nullptr
    // el clip se guarda solo con video.
    bool save_to(const std::string &path, const AVCodecParameters *video_par,
                 AVRational video_tb, const AVCodecParameters *audio_par,
                 AVRational audio_tb) const;

    int seconds() const { return seconds_; }
    size_t count() const { return video_.size(); }
    double buffered_seconds() const;

private:
    struct Entry {
        AVPacket *pkt; // copia propia (se libera con av_packet_free)
        int64_t pts_us;
    };

    static void prune(std::deque<Entry> &q, int seconds, int64_t &last_us);
    // Como prune(), pero nunca borra el ultimo keyframe anterior al corte: sin
    // el, el clip no podria empezar en un frame clave.
    static void prune_video(std::deque<Entry> &q, int seconds, int64_t last_us);

    std::deque<Entry> video_;
    std::deque<Entry> audio_;
    int seconds_;
    int64_t last_video_us_ = 0;
    int64_t last_audio_us_ = 0;
};
