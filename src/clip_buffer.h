// ============================================================================
// clip_buffer.h — buffer circular de video codificado (estilo Medal)
//
// Mientras el programa corre, cada frame se codifica y los paquetes (H.264 o
// AV1 ya comprimidos) se guardan en una cola en memoria. Lo mismo se hace con
// el audio (AAC), en una cola por pista. Las colas se podan solas para no
// crecer mas alla de unos segundos mas que la ventana pedida.
//
// Cuando el usuario aprieta el atajo se llama a save_to(): se crea un .mp4
// nuevo y se escriben los ultimos N segundos de video y audio, arrancando el
// video desde un "keyframe" para que el clip sea reproducible desde el inicio.
// ============================================================================
#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/rational.h>
}

class ClipBuffer {
public:
    // Una pista de audio del clip: sus parametros y el nombre que se ve en
    // los editores ("Mezcla", "Sistema", "Microfono").
    struct AudioTrack {
        const AVCodecParameters *par;
        std::string title;
    };

    // seconds: cuantos segundos de video se conservan.
    explicit ClipBuffer(int seconds);
    ~ClipBuffer();

    ClipBuffer(const ClipBuffer &) = delete;
    ClipBuffer &operator=(const ClipBuffer &) = delete;

    // Guarda una copia de un paquete de video. pts_us es su marca de tiempo
    // (para saber de que antiguedad es) y keyframe indica si es un frame clave.
    void push_video(const AVPacket *pkt, int64_t pts_us, bool keyframe);

    // Cuantas pistas de audio hay (1 por defecto). Cambiarlo vacia el audio.
    void set_audio_tracks(int n);
    int audio_tracks() const { return (int)audio_.size(); }

    // Igual que push_video, para un paquete de la pista de audio 'track'.
    void push_audio(int track, const AVPacket *pkt, int64_t pts_us);

    void clear();

    // Copia de lo que hay ahora en el buffer. Los datos de los paquetes se
    // comparten (conteo de referencias de FFmpeg), asi que es barata: sirve
    // para escribir el clip en otro hilo sin frenar la captura.
    std::unique_ptr<ClipBuffer> snapshot() const;

    // Escribe en 'path' los ultimos 'seconds' segundos. 'audio' tiene una
    // entrada por pista (en el mismo orden que push_audio); vacio = clip solo
    // con video. La primera pista es la que suena por defecto.
    bool save_to(const std::string &path, const AVCodecParameters *video_par,
                 AVRational video_tb, const std::vector<AudioTrack> &audio,
                 AVRational audio_tb) const;

    int seconds() const { return seconds_; }
    size_t count() const { return video_.size(); }
    double buffered_seconds() const;

private:
    struct Entry {
        AVPacket *pkt; // copia propia (se libera con av_packet_free)
        int64_t pts_us;
    };

    static void prune(std::deque<Entry> &q, int seconds, int64_t last_us);
    // Como prune(), pero nunca borra el ultimo keyframe anterior al corte: sin
    // el, el clip no podria empezar en un frame clave.
    static void prune_video(std::deque<Entry> &q, int seconds, int64_t last_us);

    std::deque<Entry> video_;
    std::vector<std::deque<Entry>> audio_ = std::vector<std::deque<Entry>>(1);
    int seconds_;
    int64_t last_video_us_ = 0;
};
