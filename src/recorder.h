// ============================================================================
// recorder.h — pega todo: captura + codificacion + buffer de clips
//
// Es el "cerebro": corre el bucle que pide frames a DXGI, los codifica con
// FFmpeg y los mete en el buffer circular. Cuando el usuario aprieta F9 guarda
// los ultimos N segundos en un .mp4.
//
// Tambien maneja el Ctrl+C (para salir limpio) y un hilo chiquito que vigila
// la tecla F9 en todo el sistema.
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
#include <string>
#include <thread>

#include "audio_capture.h"
#include "audio_encoder.h"
#include "clip_buffer.h"
#include "dxgi_capture.h"
#include "video_encoder.h"

class Recorder {
public:
    Recorder(int monitor_index, int fps, int kbps, int clip_seconds, std::string out_dir,
             bool audio_enabled = true);
    ~Recorder();

    Recorder(const Recorder &) = delete;
    Recorder &operator=(const Recorder &) = delete;

    // Corre hasta que el usuario corta con Ctrl+C. Devuelve false si algo fallo.
    bool run();

    // Ctrl+C.
    static void install_signal_handler();
    static void request_stop();
    static bool stop_requested();

private:
    bool ensure_capture();
    bool start_audio();
    void pump_audio();
    void handle_save();
    std::string make_clip_path() const;
    void hotkey_thread();
    void print_status(int64_t buffer_ms);

    DxgiCapture capture_;
    VideoEncoder encoder_;
    AudioCapture audio_capture_;
    AudioEncoder audio_encoder_;
    ClipBuffer buffer_;

    int monitor_index_;
    int fps_;
    int kbps_;
    int clip_seconds_;
    std::string out_dir_;
    bool audio_enabled_;

    std::thread hotkey_;
    std::atomic<bool> save_requested_{false};

    bool capture_started_ = false;
    bool failed_ = false;
    int64_t start_time_us_ = -1;
    int64_t audio_open_us_ = 0;
    int64_t frame_count_ = 0;
};
