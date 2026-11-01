// ============================================================================
// recorder.h — pega todo: captura + codificacion + buffer de clips
//
// Es el "cerebro": corre el bucle que pide frames a DXGI, los codifica con
// FFmpeg, captura el audio y los mete en el buffer circular. Cuando se pide
// guardar (tecla F9 o el boton de la interfaz) escribe los ultimos N segundos
// en un .mp4.
//
// La interfaz grafica lo corre en un hilo aparte: cada Recorder vive lo que
// dura una grabacion (se crea al apretar "Iniciar" y se destruye al
// "Detener"), y consulta su estado con is_running()/frames()/buffered_seconds().
// ============================================================================
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <functional>
#include <string>
#include <thread>

#include "audio_capture.h"
#include "audio_encoder.h"
#include "clip_buffer.h"
#include "config.h"
#include "dxgi_capture.h"
#include "video_encoder.h"

class Recorder {
public:
    // Mensajes de estado para la interfaz (se llaman desde el hilo de captura).
    using LogFn = std::function<void(const std::string &)>;

    explicit Recorder(const AppConfig &cfg);
    ~Recorder();

    Recorder(const Recorder &) = delete;
    Recorder &operator=(const Recorder &) = delete;

    // Corre hasta que alguien pida detener (request_stop). Devuelve false si
    // algo fallo antes de arrancar.
    bool run();

    // Pide guardar un clip (equivalente a apretar F9).
    void request_save() { save_requested_.store(true); }

    // Se llama con los mensajes de estado (guardado, errores, etc).
    void set_log_callback(LogFn fn) { log_cb_ = std::move(fn); }

    bool is_running() const { return running_.load(); }
    long frames() const { return frames_.load(); }
    double buffered_seconds() const { return buffer_seconds_.load(); }
    bool failed() const { return failed_.load(); }

    // Ctrl+C (la version de consola).
    static void install_signal_handler();
    static void request_stop();
    static void clear_stop();
    static bool stop_requested();

private:
    bool ensure_capture();
    bool start_audio();
    void pump_audio();
    void handle_save();
    std::string make_clip_path() const;
    void hotkey_thread();
    void print_status(double buffer_s);
    void log(const std::string &msg);

    DxgiCapture capture_;
    VideoEncoder encoder_;
    AudioCapture audio_capture_;
    AudioEncoder audio_encoder_;
    ClipBuffer buffer_;

    AppConfig cfg_;
    bool audio_enabled_;

    std::thread hotkey_;
    std::atomic<bool> save_requested_{false};

    bool capture_started_ = false;
    std::atomic<bool> failed_{false};
    std::atomic<bool> running_{false};
    int64_t start_time_us_ = -1;
    int64_t audio_open_us_ = 0;
    std::atomic<long> frames_{0};
    std::atomic<double> buffer_seconds_{0.0};

    LogFn log_cb_;
};
