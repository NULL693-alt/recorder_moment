// ============================================================================
// audio_capture.h — captura de audio (WASAPI): sistema + microfono mezclados
//
// Captura al mismo tiempo:
//   * el "loopback" del dispositivo de salida (todo lo que suena: juego,
//     musica, Discord, videos...), y
//   * el microfono por defecto.
//
// Los dos se convierten al mismo formato (por defecto 48000 Hz, estereo, 16
// bits) y se mezclan en una sola pista. El resultado queda en un FIFO que el
// grabador va sacando.
//
// La mezcla se produce a ritmo fijo (tiempo real) para que el audio no se
// desincronice del video aunque el dispositivo de salida entre en reposo
// (cuando no suena nada, Windows deja de mandar datos: en ese caso se usa
// silencio para esa fuente).
//
// Si no hay microfono, sigue andando solo con el audio del sistema.
// ============================================================================
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <mmdeviceapi.h>
#include <audioclient.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

extern "C" {
#include <libswresample/swresample.h>
}

class AudioCapture {
public:
    AudioCapture() = default;
    ~AudioCapture();

    AudioCapture(const AudioCapture &) = delete;
    AudioCapture &operator=(const AudioCapture &) = delete;

    // Abre el loopback del sistema y (si hay) el microfono, y arranca la
    // captura en un hilo aparte. Devuelve false solo si no se pudo capturar
    // el audio del sistema.
    bool open(int sample_rate = 48000, int channels = 2);
    void close();

    // Saca hasta max_frames frames mezclados (16 bits intercalados). Devuelve
    // cuantos frames copio.
    int read(int16_t *dst, int max_frames);

    bool is_open() const { return running_.load(); }
    bool has_mic() const { return has_mic_; }
    int sample_rate() const { return sample_rate_; }
    int channels() const { return channels_; }

    // Cuantos frames hay listos para leer.
    int available_frames() const;

private:
    // Un endpoint: el loopback del sistema o el microfono.
    struct Source {
        IMMDevice *device = nullptr;
        IAudioClient *client = nullptr;
        IAudioCaptureClient *capture = nullptr;
        WAVEFORMATEX *format = nullptr;
        SwrContext *swr = nullptr;
        HANDLE event = nullptr;
        // Frames ya convertidos a 16 bits estereo, esperando para mezclarse.
        std::vector<int16_t> fifo;
        std::vector<uint8_t> zero_in; // buffer de ceros para los paquetes mudos
        bool ok = false;
    };

    bool open_source(Source &s, EDataFlow flow, bool loopback);
    void close_source(Source &s);
    void thread_main();
    void drain(Source &s);
    void push_frames(Source &s, const uint8_t *data, UINT32 frames, bool silent);
    void produce_locked(); // requiere mutex_

    Source sys_;
    Source mic_;

    std::thread thread_;
    HANDLE stop_event_ = nullptr;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_{false};
    bool com_inited_ = false;

    int sample_rate_ = 48000;
    int channels_ = 2;
    bool has_mic_ = false;

    mutable std::mutex mutex_;
    std::vector<int16_t> out_fifo_; // mezcla lista para leer
    int64_t start_us_ = 0;
    int64_t produced_frames_ = 0;
};
