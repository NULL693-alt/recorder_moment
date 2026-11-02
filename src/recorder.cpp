#include "recorder.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <vector>

// ---------------------------------------------------------------------------
// Parada (Ctrl+C en consola, o "Detener" en la interfaz).
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
void Recorder::clear_stop() { g_stop.store(false); }
bool Recorder::stop_requested() { return g_stop.load(); }

static int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// ---------------------------------------------------------------------------
Recorder::Recorder(const AppConfig &cfg) : buffer_(cfg.clip_seconds), cfg_(cfg) {
    if (cfg_.fps <= 0) cfg_.fps = 60;
    if (cfg_.kbps <= 0) cfg_.kbps = 12000;
    if (cfg_.clip_seconds <= 0) cfg_.clip_seconds = 120;
    if (cfg_.out_dir.empty()) cfg_.out_dir = ".";
    audio_enabled_ = cfg_.audio_enabled;
}

Recorder::~Recorder() {
    if (hotkey_.joinable()) hotkey_.join();
    join_save();
}

void Recorder::join_save() {
    if (save_thread_.joinable()) save_thread_.join();
}

// ---------------------------------------------------------------------------
// Mensajes: van a la consola (si hay) y a la interfaz (si hay).
// ---------------------------------------------------------------------------
void Recorder::log(const std::string &msg) {
    fprintf(stderr, "%s\n", msg.c_str());
    if (log_cb_) log_cb_(msg);
}

// ---------------------------------------------------------------------------
// Reconectar la captura (tambien sirve para reintentar cuando el sistema esta
// ocupado con otras capturas: OBS, Discord, Game Bar).
// ---------------------------------------------------------------------------
bool Recorder::ensure_capture() {
    bool first = !capture_started_;
    const int attempts = 5;
    for (int i = 0; i < attempts; i++) {
        if (capture_.init(cfg_.monitor, first)) {
            capture_started_ = true;
            return true;
        }
        if (i + 1 < attempts) {
            log("[captura] reintentando en 1s (" + std::to_string(i + 1) + "/" +
                std::to_string(attempts) + ")...");
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

    float mic_gain = (float)cfg_.mic_volume / 100.0f;
    if (!audio_capture_.open(48000, 2, cfg_.output_device, cfg_.mic_device,
                             cfg_.mic_enabled, mic_gain) ||
        !audio_encoder_.open(audio_capture_.sample_rate(), audio_capture_.channels(),
                             cfg_.audio_kbps)) {
        log("[audio] no se pudo iniciar el audio; se graba solo video");
        audio_encoder_.close();
        audio_capture_.close();
        audio_enabled_ = false;
        return false;
    }
    audio_next_index_ = 0;
    return true;
}

// ---------------------------------------------------------------------------
// Audio: saca lo que haya del dispositivo, lo codifica y lo mete en el buffer
// circular. Los paquetes llevan la misma linea de tiempo (microsegundos) que
// el video para que el clip quede sincronizado.
// ---------------------------------------------------------------------------
void Recorder::pump_audio() {
    if (!audio_enabled_ || !audio_encoder_.is_open()) return;
    if (start_time_us_ < 0) return; // todavia no arranco el bucle

    const int sr = audio_capture_.sample_rate();
    const int ch = audio_capture_.channels();
    const int kChunk = sr / 10; // hasta 100 ms por vuelta
    std::vector<int16_t> buf((size_t)kChunk * ch);

    // La muestra N del codificador es la muestra N de la captura, que se tomo
    // en audio_capture_.start_us() + N/sr: de ahi sale su marca de tiempo.
    const int64_t audio_origin_us = audio_capture_.start_us() - start_time_us_;
    auto on_packet = [this, sr, audio_origin_us](const AVPacket *pkt) {
        int64_t pts_us = audio_origin_us + (int64_t)pkt->pts * 1000000 / sr;
        buffer_.push_audio(pkt, pts_us);
    };

    int got;
    int64_t first = 0;
    while ((got = audio_capture_.read(buf.data(), kChunk, &first)) > 0) {
        // Si la captura tuvo que descartar muestras (el bucle se freno mucho,
        // por ejemplo reconectando la pantalla), rellenamos el hueco con
        // silencio: asi la cuenta de muestras sigue pegada al reloj y el audio
        // no queda corrido el resto de la grabacion.
        if (first > audio_next_index_) {
            std::vector<int16_t> zeros((size_t)kChunk * ch, 0);
            int64_t gap = first - audio_next_index_;
            while (gap > 0) {
                int n = (int)(gap < kChunk ? gap : kChunk);
                audio_encoder_.encode(zeros.data(), n, on_packet);
                gap -= n;
            }
        }
        audio_next_index_ = first + got;
        audio_encoder_.encode(buf.data(), got, on_packet);
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
    std::string dir = cfg_.out_dir;
    if (!dir.empty() && dir.back() != '\\' && dir.back() != '/') dir += '\\';

    // Dos clips en el mismo segundo no se pisan: el segundo lleva _2, _3...
    std::string base = dir + name;
    std::string path = base;
    std::error_code ec;
    for (int i = 2; std::filesystem::exists(std::filesystem::u8path(path), ec) && i < 100; i++)
        path = base.substr(0, base.size() - 4) + "_" + std::to_string(i) + ".mp4";
    return path;
}

// ---------------------------------------------------------------------------
// Guardar el clip
// ---------------------------------------------------------------------------
void Recorder::handle_save() {
    if (!encoder_.is_open() || buffer_.count() == 0) {
        log("[clip] todavia no hay video capturado para guardar");
        return;
    }
    if (saving_.load()) {
        log("[clip] todavia se esta guardando el clip anterior; proba de nuevo en un momento");
        return;
    }
    join_save(); // el hilo del clip anterior ya termino: lo juntamos

    // Todo lo que usa el hilo de guardado va copiado, porque la captura sigue
    // llenando el buffer y usando los codificadores mientras tanto.
    AVCodecParameters *vpar = avcodec_parameters_alloc();
    if (!vpar || avcodec_parameters_copy(vpar, encoder_.codecpar()) < 0) {
        avcodec_parameters_free(&vpar);
        log("[clip] no se pudo guardar el clip");
        return;
    }
    AVCodecParameters *apar = nullptr;
    if (audio_enabled_ && audio_encoder_.is_open()) {
        apar = avcodec_parameters_alloc();
        if (apar && avcodec_parameters_copy(apar, audio_encoder_.codecpar()) < 0)
            avcodec_parameters_free(&apar);
    }
    const AVRational vtb = encoder_.time_base();
    const AVRational atb = audio_encoder_.time_base();
    std::unique_ptr<ClipBuffer> snap = buffer_.snapshot();

    std::string path = make_clip_path();
    double secs = buffer_.buffered_seconds();
    if (secs > cfg_.clip_seconds) secs = cfg_.clip_seconds;
    log("[clip] guardando ultimos " + std::to_string((int)secs) + " s en " + path + " ...");

    saving_.store(true);
    save_thread_ = std::thread([this, snap = std::move(snap), vpar, apar, vtb, atb,
                                path]() mutable {
        bool ok = snap->save_to(path, vpar, vtb, apar, atb);
        snap.reset();
        avcodec_parameters_free(&vpar);
        if (apar) avcodec_parameters_free(&apar);
        log(ok ? "[clip] listo: " + path : std::string("[clip] no se pudo guardar el clip"));
        saving_.store(false);
    });
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
// Linea de estado (solo consola)
// ---------------------------------------------------------------------------
void Recorder::print_status(double buffer_s) {
    fprintf(stderr, "\r[grabando] %ld frames | buffer %.0f s / %d s | F9 = guardar clip | "
                    "Ctrl+C = salir   ",
            frames_.load(), buffer_s, cfg_.clip_seconds);
    fflush(stderr);
}

// ---------------------------------------------------------------------------
// Bucle principal
// ---------------------------------------------------------------------------
bool Recorder::run() {
    if (!ensure_capture()) {
        log("No se pudo iniciar la captura de pantalla.");
        return false;
    }

    log("Capturando el monitor #" + std::to_string(cfg_.monitor) +
        (audio_enabled_ ? " con audio." : " sin audio."));

    start_audio();
    running_.store(true);

    hotkey_ = std::thread(&Recorder::hotkey_thread, this);

    // La linea de tiempo arranca ya (no con el primer frame), asi el audio se
    // empieza a codificar de una aunque la pantalla tarde en cambiar.
    start_time_us_ = now_us();
    const int64_t frame_interval_us = 1000000 / cfg_.fps;

    while (!Recorder::stop_requested()) {
        // Esperamos un frame nuevo como mucho hasta que toque el proximo: si no
        // llega, se repite el ultimo (ver mas abajo).
        int timeout_ms = 200;
        if (encoder_.is_open() && next_frame_us_ >= 0) {
            int64_t wait_ms = (next_frame_us_ - now_us()) / 1000;
            timeout_ms = (int)(wait_ms < 0 ? 0 : (wait_ms > 200 ? 200 : wait_ms));
        }
        DxgiCapture::Result r = capture_.acquire(timeout_ms);

        if (r == DxgiCapture::Result::AccessLost) {
            log("[captura] se perdio la duplicacion, reconectando...");
            if (!ensure_capture()) {
                failed_.store(true);
                break;
            }
            continue;
        }
        if (r == DxgiCapture::Result::Error) {
            failed_.store(true);
            break;
        }

        // Limite de FPS: DXGI entrega un frame por cada refresco del monitor
        // (144 Hz, 240 Hz...). Solo codificamos los que tocan segun cfg_.fps; el
        // margen de 1/8 de frame absorbe el jitter.
        int64_t t = now_us();
        bool due = next_frame_us_ < 0 || t >= next_frame_us_ - frame_interval_us / 8;

        // FPS constantes: Windows solo manda frames cuando la pantalla cambia.
        // Si ya toca un frame y no llego ninguno, repetimos el ultimo capturado
        // (aunque se haya salteado por el limite de FPS, asi tampoco queda
        // afuera el ultimo cambio). Un frame repetido pesa casi nada.
        bool fresh = (r == DxgiCapture::Result::Frame);
        bool repeat = !fresh && due && encoder_.is_open() && capture_.remap_last();

        if ((fresh && due) || repeat) {
            // El proximo toca un intervalo despues del anterior, pero nunca a
            // menos de 3/4 de intervalo de este: si un frame llego tarde no se
            // compensa con uno pegado (eso se ve como tirones).
            int64_t base = next_frame_us_ < 0 ? t : next_frame_us_;
            next_frame_us_ = base + frame_interval_us;
            if (next_frame_us_ < t + frame_interval_us * 3 / 4)
                next_frame_us_ = t + frame_interval_us * 3 / 4;

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
                    !encoder_.open(capture_.width(), capture_.height(), cfg_.fps, cfg_.kbps,
                                   fmt, cfg_.out_height, capture_.sdr_white_scale())) {
                    failed_.store(true);
                    break;
                }
            }

            int64_t pts = t - start_time_us_;

            bool ok = encoder_.encode(capture_.data(), capture_.row_pitch(), pts,
                                      [this](const AVPacket *pkt) {
                                          buffer_.push_video(
                                              pkt, pkt->pts,
                                              (pkt->flags & AV_PKT_FLAG_KEY) != 0);
                                      });
            if (!ok) {
                failed_.store(true);
                break;
            }
            frames_.fetch_add(1);

            double buf_s = buffer_.buffered_seconds();
            buffer_seconds_.store(buf_s);
            if (frames_.load() % 60 == 0) print_status(buf_s);
        }

        // El audio corre aparte del video: se bombea en cada vuelta para que no
        // se acumule aunque la pantalla no cambie.
        pump_audio();

        // El atajo se revisa SIEMPRE, aunque la pantalla no haya cambiado.
        if (save_requested_.exchange(false)) handle_save();
    }

    running_.store(false);
    Recorder::request_stop();
    if (hotkey_.joinable()) hotkey_.join();
    join_save(); // si quedo un clip escribiendose, lo dejamos terminar
    audio_capture_.close();
    audio_encoder_.close();

    fprintf(stderr, "\n");
    if (!failed_.load() && frames_.load() == 0)
        log("No se capturo nada (no hubo cambios en la pantalla).");
    else if (failed_.load())
        log("Algo fallo durante la grabacion.");
    else
        log("Grabacion detenida.");
    return !failed_.load();
}
