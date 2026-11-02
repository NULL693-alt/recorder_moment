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
#include "gpu_convert.h"
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

    // Se llama cada vez que termina un pedido de clip: ok = se guardo (msg es
    // la ruta del archivo) o no (msg dice por que). Puede llamarse desde otro
    // hilo.
    using ClipFn = std::function<void(bool ok, const std::string &msg)>;
    void set_clip_callback(ClipFn fn) { clip_cb_ = std::move(fn); }

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
    bool ensure_converter();
    bool start_audio();
    void pump_audio();
    void handle_save();
    std::string make_clip_path() const;
    void join_save();
    void hotkey_thread();
    void print_status(double buffer_s);
    void log(const std::string &msg);

    DxgiCapture capture_;
    GpuConverter converter_;
    int converter_generation_ = 0; // generation() de la captura sobre la que se armo
    int cursor_version_ = -1;      // forma del cursor que ya tiene converter_
    VideoEncoder encoder_;
    AudioCapture audio_capture_;
    AudioEncoder audio_encoder_;
    ClipBuffer buffer_;

    AppConfig cfg_;
    bool audio_enabled_;

    std::thread hotkey_;
    std::atomic<bool> save_requested_{false};

    // El clip se escribe en un hilo aparte para no frenar la captura.
    std::thread save_thread_;
    std::atomic<bool> saving_{false};

    bool capture_started_ = false;
    std::atomic<bool> failed_{false};
    std::atomic<bool> running_{false};
    int64_t start_time_us_ = -1;
    int64_t next_frame_us_ = -1;    // cuando toca el proximo frame (limite de FPS)
    int64_t audio_next_index_ = 0;  // indice de la proxima muestra de audio esperada
    std::atomic<long> frames_{0};
    std::atomic<double> buffer_seconds_{0.0};

    LogFn log_cb_;
    ClipFn clip_cb_;
};
