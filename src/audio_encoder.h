// ============================================================================
// audio_encoder.h — codificacion de audio (AAC) con FFmpeg
//
// Recibe PCM 16 bits intercalado (lo que entrega AudioCapture) y produce
// paquetes AAC listos para meter en el .mp4. No escribe archivos: entrega cada
// paquete por un callback; quien decide donde guardarlo es el ClipBuffer.
//
// El codificador AAC trabaja en frames de tamano fijo (1024 muestras), asi que
// esta clase acumula lo que sobra entre llamadas.
// ============================================================================
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/rational.h>
#include <libswresample/swresample.h>
}

class AudioEncoder {
public:
    // Se llama por cada paquete codificado. El AVPacket es valido solo durante
    // la llamada: hay que copiarlo si se quiere conservar.
    using PacketFn = std::function<void(const AVPacket *pkt)>;

    AudioEncoder() = default;
    ~AudioEncoder();

    AudioEncoder(const AudioEncoder &) = delete;
    AudioEncoder &operator=(const AudioEncoder &) = delete;

    bool open(int sample_rate, int channels, int bitrate_kbps);
    void close();

    // Recibe 'frames' frames PCM 16 bits intercalados. Devuelve false si el
    // codificador fallo.
    bool encode(const int16_t *data, int frames, const PacketFn &on_packet);

    // Vacia lo que quede dentro del codificador al terminar.
    void flush(const PacketFn &on_packet);

    AVCodecParameters *codecpar() const { return codecpar_; }
    AVRational time_base() const { return time_base_; }
    int sample_rate() const { return sample_rate_; }
    bool is_open() const { return codec_ctx_ != nullptr; }

private:
    bool feed(const PacketFn &on_packet);
    void drain(const PacketFn &on_packet);

    AVCodecContext *codec_ctx_ = nullptr;
    AVCodecParameters *codecpar_ = nullptr;
    SwrContext *swr_ = nullptr;
    AVFrame *frame_ = nullptr;
    AVPacket *pkt_ = nullptr;

    int sample_rate_ = 48000;
    int channels_ = 2;
    AVRational time_base_{1, 48000};
    int64_t next_pts_ = 0;
    std::vector<int16_t> pending_; // muestras que todavia no completan un frame
};
