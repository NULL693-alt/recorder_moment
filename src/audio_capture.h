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
// silencio para esa fuente). Va un margen fijo (kLatencyUs) por detras del
// reloj, asi los paquetes que llegan un poco tarde no dejan huecos (chasquidos),
// y la deriva entre el reloj de la placa de sonido y el de la PC se corrige
// estirando o encogiendo el audio un poquito (swr_set_compensation).
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
#include <string>
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

    // Abre el loopback del sistema y (si enable_mic) el microfono, y arranca la
    // captura en un hilo aparte. Devuelve false solo si no se pudo capturar
    // el audio del sistema. sys_id/mic_id son ids de WASAPI; vacio = el
    // dispositivo predeterminado. mic_gain es el volumen del microfono
    // (1.0 = 100%).
    bool open(int sample_rate = 48000, int channels = 2,
              const std::string &sys_id = std::string(),
              const std::string &mic_id = std::string(), bool enable_mic = true,
              float mic_gain = 1.0f);
    void close();

    // Saca hasta max_frames frames mezclados (16 bits intercalados). Devuelve
    // cuantos frames copio. Si first_index no es nullptr, recibe el numero de
    // muestra (desde open) del primer frame copiado: si es mayor que el
    // esperado es que se descartaron muestras porque nadie las leia a tiempo.
    int read(int16_t *dst, int max_frames, int64_t *first_index = nullptr);

    // Instante (reloj steady_clock, en microsegundos) en que se capturo la
    // muestra 0. La muestra N es de start_us() + N / sample_rate().
    int64_t start_us() const { return start_us_; }

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
        // false mientras se junta el margen de latencia (al arrancar o despues
        // de quedarse sin datos); mientras tanto esta fuente aporta silencio.
        bool primed = false;
        bool compensating = false;  // hay una correccion de deriva activa en swr
        int64_t last_drift_us = 0;  // ultima vez que se reviso la deriva
    };

    bool open_source(Source &s, EDataFlow flow, bool loopback, const std::string &device_id);
    void close_source(Source &s);
    void thread_main();
    void drain(Source &s);
    void push_frames(Source &s, const uint8_t *data, UINT32 frames, bool silent);
    void produce_locked(); // requiere mutex_
    void adjust_drift(Source &s);

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
    float mic_gain_ = 1.0f; // volumen del microfono (1.0 = 100%)

    mutable std::mutex mutex_;
    std::vector<int16_t> out_fifo_; // mezcla lista para leer
    int64_t start_us_ = 0;
    int64_t produced_frames_ = 0;
};
