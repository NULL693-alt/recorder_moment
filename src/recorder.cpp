#include "recorder.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <vector>

// ---------------------------------------------------------------------------
// Ctrl+C
// ---------------------------------------------------------------------------
static std::atomic<bool> g_stop{false};

static BOOL WINAPI ctrl_handler(DWORD type) {
    switch (type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
            g_stop.store(true);
            return TRUE;
        default:
            return FALSE;
    }
}

void Recorder::install_signal_handler() { SetConsoleCtrlHandler(ctrl_handler, TRUE); }
void Recorder::request_stop() { g_stop.store(true); }
bool Recorder::stop_requested() { return g_stop.load(); }

static int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// ---------------------------------------------------------------------------
Recorder::Recorder(int monitor_index, int fps, int kbps, int clip_seconds,
                   std::string out_dir, bool audio_enabled)
    : buffer_(clip_seconds),
      monitor_index_(monitor_index),
      fps_(fps > 0 ? fps : 60),
      kbps_(kbps > 0 ? kbps : 12000),
      clip_seconds_(clip_seconds > 0 ? clip_seconds : 120),
      out_dir_(std::move(out_dir)),
      audio_enabled_(audio_enabled) {}

Recorder::~Recorder() {
    if (hotkey_.joinable()) hotkey_.join();
}

// ---------------------------------------------------------------------------
// Reconectar la captura (tambien sirve para reintentar cuando el sistema esta
// ocupado con otras capturas: OBS, Discord, Game Bar).
// ---------------------------------------------------------------------------
bool Recorder::ensure_capture() {
    bool first = !capture_started_;
    const int attempts = 5;
    for (int i = 0; i < attempts; i++) {
        if (capture_.init(monitor_index_, first)) {
            capture_started_ = true;
            return true;
        }
        if (i + 1 < attempts) {
            fprintf(stderr, "[captura] reintentando en 1s (%d/%d)...\n", i + 1, attempts);
            Sleep(1000);
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Audio: abre la captura (loopback del sistema + microfono) y el codificador
// AAC. Si algo falla, el programa sigue andando solo con video.
// ---------------------------------------------------------------------------
bool Recorder::start_audio() {
    if (!audio_enabled_) return false;

    if (!audio_capture_.open(48000, 2) ||
        !audio_encoder_.open(audio_capture_.sample_rate(), audio_capture_.channels(), 160)) {
        fprintf(stderr, "[audio] no se pudo iniciar el audio; se graba solo video\n");
        audio_encoder_.close();
        audio_capture_.close();
        audio_enabled_ = false;
        return false;
    }
    audio_open_us_ = now_us();
    return true;
}

// ---------------------------------------------------------------------------
// Audio: saca lo que haya del dispositivo, lo codifica y lo mete en el buffer
// circular. Los paquetes llevan la misma linea de tiempo (microsegundos) que
// el video para que el clip quede sincronizado.
// ---------------------------------------------------------------------------
void Recorder::pump_audio() {
    if (!audio_enabled_ || !audio_encoder_.is_open()) return;
    if (start_time_us_ < 0) return; // todavia no arranco el video

    const int sr = audio_capture_.sample_rate();
    const int ch = audio_capture_.channels();
    const int kChunk = sr / 10; // hasta 100 ms por vuelta
    std::vector<int16_t> buf((size_t)kChunk * ch);

    int got;
    while ((got = audio_capture_.read(buf.data(), kChunk)) > 0) {
        audio_encoder_.encode(buf.data(), got, [this, sr](const AVPacket *pkt) {
            int64_t pts_us =
                audio_open_us_ - start_time_us_ + (int64_t)pkt->pts * 1000000 / sr;
            buffer_.push_audio(pkt, pts_us);
        });
    }
}

// ---------------------------------------------------------------------------
// Nombre del archivo del clip
// ---------------------------------------------------------------------------
std::string Recorder::make_clip_path() const {
    std::time_t t = std::time(nullptr);
    std::tm tm_buf{};
    localtime_s(&tm_buf, &t);
    char name[64];
    std::snprintf(name, sizeof(name), "clip_%04d%02d%02d_%02d%02d%02d.mp4",
                  tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                  tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
    std::string dir = out_dir_;
    if (!dir.empty() && dir.back() != '\\' && dir.back() != '/') dir += '\\';
    return dir + name;
}

// ---------------------------------------------------------------------------
// Guardar el clip
// ---------------------------------------------------------------------------
void Recorder::handle_save() {
    if (!encoder_.is_open() || buffer_.count() == 0) {
        fprintf(stderr, "\n[clip] todavia no hay video capturado para guardar\n");
        return;
    }
    std::string path = make_clip_path();
    double secs = buffer_.buffered_seconds();
    const AVCodecParameters *apar =
        (audio_enabled_ && audio_encoder_.is_open()) ? audio_encoder_.codecpar() : nullptr;
    fprintf(stderr, "\n[clip] guardando ultimos %.0f s en %s ...\n", secs, path.c_str());
    if (buffer_.save_to(path, encoder_.codecpar(), encoder_.time_base(), apar,
                        audio_encoder_.time_base()))
        fprintf(stderr, "[clip] listo: %s (%.0f s, %ld frames)\n", path.c_str(), secs,
                (long)frame_count_);
    else
        fprintf(stderr, "[clip] no se pudo guardar el clip\n");
}

// ---------------------------------------------------------------------------
// Vigilancia de la tecla F9 (funciona aunque el juego tenga el foco, siempre
// que no este en pantalla completa exclusiva).
// ---------------------------------------------------------------------------
void Recorder::hotkey_thread() {
    bool prev_down = false;
    while (!Recorder::stop_requested()) {
        bool down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
        if (down && !prev_down) save_requested_.store(true);
        prev_down = down;
        Sleep(25);
    }
}

// ---------------------------------------------------------------------------
// Linea de estado
// ---------------------------------------------------------------------------
void Recorder::print_status(int64_t buffer_ms) {
    fprintf(stderr, "\r[grabando] %ld frames | buffer %.0f s / %d s | F9 = guardar clip | "
                    "Ctrl+C = salir   ",
            (long)frame_count_, buffer_ms / 1000.0, clip_seconds_);
    fflush(stderr);
}

// ---------------------------------------------------------------------------
// Bucle principal
// ---------------------------------------------------------------------------
bool Recorder::run() {
    if (!ensure_capture()) {
        fprintf(stderr, "\nNo se pudo iniciar la captura de pantalla.\n");
        return false;
    }

    fprintf(stderr,
            "Capturando el monitor #%d. %s Apreta F9 para guardar los ultimos %d s "
            "en un clip. Ctrl+C para salir.\n",
            monitor_index_,
            audio_enabled_ ? "Con audio (sistema + microfono)." : "Sin audio.", clip_seconds_);

    start_audio();

    hotkey_ = std::thread(&Recorder::hotkey_thread, this);

    while (!Recorder::stop_requested()) {
        DxgiCapture::Result r = capture_.acquire(200);

        if (r == DxgiCapture::Result::AccessLost) {
            fprintf(stderr, "\n[captura] se perdio la duplicacion, reconectando...\n");
            if (!ensure_capture()) {
                failed_ = true;
                break;
            }
            continue;
        }
        if (r == DxgiCapture::Result::Error) {
            failed_ = true;
            break;
        }

        if (r == DxgiCapture::Result::Frame) {
            // El codificador se arma con el primer frame, cuando ya sabemos el
            // tamano real del monitor y que formato entrega.
            if (!encoder_.is_open()) {
                AVPixelFormat fmt;
                switch (capture_.format()) {
                    case CaptureFormat::BGRA: fmt = AV_PIX_FMT_BGRA; break;
                    case CaptureFormat::RGBA: fmt = AV_PIX_FMT_RGBA; break;
                    case CaptureFormat::HDR_FLOAT: fmt = AV_PIX_FMT_RGBAF16; break;
                    default: fmt = AV_PIX_FMT_NONE; break;
                }
                if (fmt == AV_PIX_FMT_NONE ||
                    !encoder_.open(capture_.width(), capture_.height(), fps_, kbps_, fmt)) {
                    failed_ = true;
                    break;
                }
            }

            int64_t t = now_us();
            if (start_time_us_ < 0) start_time_us_ = t;
            int64_t pts = t - start_time_us_;

            bool ok = encoder_.encode(capture_.data(), capture_.row_pitch(), pts,
                                      [this](const AVPacket *pkt) {
                                          buffer_.push_video(
                                              pkt, pkt->pts,
                                              (pkt->flags & AV_PKT_FLAG_KEY) != 0);
                                      });
            if (!ok) {
                failed_ = true;
                break;
            }
            frame_count_++;

            if (frame_count_ % 60 == 0)
                print_status((int64_t)buffer_.buffered_seconds() * 1000);
        }

        // El audio corre aparte del video: se bombea en cada vuelta para que no
        // se acumule aunque la pantalla no cambie.
        pump_audio();

        // El atajo se revisa SIEMPRE, aunque la pantalla no haya cambiado.
        if (save_requested_.exchange(false)) handle_save();
    }

    Recorder::request_stop();
    if (hotkey_.joinable()) hotkey_.join();
    audio_capture_.close();
    audio_encoder_.close();

    fprintf(stderr, "\n");
    if (!failed_ && frame_count_ == 0) {
        fprintf(stderr, "No se capturo nada (no hubo cambios en la pantalla).\n");
        return false;
    }
    if (failed_) {
        fprintf(stderr, "Algo fallo durante la grabacion. Revisa el mensaje de arriba.\n");
        return false;
    }
    fprintf(stderr, "Listo. Saliste despues de %ld frames.\n", (long)frame_count_);
    return true;
}
