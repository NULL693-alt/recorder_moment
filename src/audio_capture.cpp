#include "audio_capture.h"

#include <chrono>
#include <cstdio>
#include <cstring>

#include <objbase.h>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/mathematics.h>
#include <libavutil/samplefmt.h>
}

// Clase del enumerador de dispositivos de audio. La definimos nosotros para no
// depender de que MinGW traiga el simbolo CLSID_MMDeviceEnumerator.
static const CLSID kClsidMMDeviceEnumerator = {
    0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};

// Subtipos de WAVE_FORMAT_EXTENSIBLE (PCM entero y float).
static const GUID kSubtypePcm = {
    0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
static const GUID kSubtypeFloat = {
    0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

// Margen con el que la mezcla va por detras del reloj (y nivel objetivo de
// cada FIFO de entrada). WASAPI entrega paquetes cada ~10 ms con algo de
// jitter: 100 ms sobra para que nunca falten datos al mezclar.
static const int64_t kLatencyUs = 100000;

static int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Los ids de WASAPI son ASCII, pero por las dudas convertimos bien.
static std::wstring utf8_to_wide(const std::string &s) {
    if (s.empty()) return std::wstring();
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring out((size_t)len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], len);
    return out;
}

// Traduce el formato que entrega WASAPI al que entiende swresample.
static AVSampleFormat wave_to_av(const WAVEFORMATEX *fmt) {
    if (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return AV_SAMPLE_FMT_FLT;
    if (fmt->wFormatTag == WAVE_FORMAT_PCM) {
        return fmt->wBitsPerSample == 32 ? AV_SAMPLE_FMT_S32 : AV_SAMPLE_FMT_S16;
    }
    if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const WAVEFORMATEXTENSIBLE *ext = (const WAVEFORMATEXTENSIBLE *)fmt;
        if (IsEqualGUID(ext->SubFormat, kSubtypeFloat)) return AV_SAMPLE_FMT_FLT;
        if (IsEqualGUID(ext->SubFormat, kSubtypePcm)) {
            return fmt->wBitsPerSample == 32 ? AV_SAMPLE_FMT_S32 : AV_SAMPLE_FMT_S16;
        }
    }
    return AV_SAMPLE_FMT_NONE;
}

AudioCapture::~AudioCapture() { close(); }

// ---------------------------------------------------------------------------
// Abrir un endpoint
// ---------------------------------------------------------------------------
bool AudioCapture::open_source(Source &s, EDataFlow flow, bool loopback,
                               const std::string &device_id) {
    IMMDeviceEnumerator *enumerator = nullptr;
    HRESULT hr = CoCreateInstance(kClsidMMDeviceEnumerator, nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator), (void **)&enumerator);
    if (FAILED(hr)) return false;

    if (device_id.empty()) {
        hr = enumerator->GetDefaultAudioEndpoint(flow, eConsole, &s.device);
    } else {
        std::wstring wid = utf8_to_wide(device_id);
        hr = enumerator->GetDevice(wid.c_str(), &s.device);
        if (FAILED(hr)) // si el dispositivo guardado ya no existe, usamos el predeterminado
            hr = enumerator->GetDefaultAudioEndpoint(flow, eConsole, &s.device);
    }
    enumerator->Release();
    if (FAILED(hr)) return false;

    hr = s.device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void **)&s.client);
    if (FAILED(hr)) return false;

    hr = s.client->GetMixFormat(&s.format);
    if (FAILED(hr)) return false;

    AVSampleFormat in_fmt = wave_to_av(s.format);
    if (in_fmt == AV_SAMPLE_FMT_NONE) {
        fprintf(stderr, "[audio] formato de dispositivo no soportado (tag=%u, %u bits)\n",
                (unsigned)s.format->wFormatTag, (unsigned)s.format->wBitsPerSample);
        return false;
    }

    DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    if (loopback) flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;

    hr = s.client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 0, 0, s.format, nullptr);
    if (FAILED(hr)) {
        fprintf(stderr, "[audio] Initialize fallo (0x%08lx)\n", (unsigned long)hr);
        return false;
    }

    s.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!s.event || FAILED(s.client->SetEventHandle(s.event))) {
        fprintf(stderr, "[audio] no pude preparar el evento de captura\n");
        return false;
    }

    hr = s.client->GetService(__uuidof(IAudioCaptureClient), (void **)&s.capture);
    if (FAILED(hr)) {
        fprintf(stderr, "[audio] no pude obtener IAudioCaptureClient (0x%08lx)\n",
                (unsigned long)hr);
        return false;
    }

    // Conversor: formato del dispositivo -> 16 bits estereo a la tasa pedida.
    AVChannelLayout out_layout;
    av_channel_layout_default(&out_layout, channels_);
    AVChannelLayout in_layout;
    av_channel_layout_default(&in_layout, s.format->nChannels);

    int rc = swr_alloc_set_opts2(&s.swr, &out_layout, AV_SAMPLE_FMT_S16, sample_rate_,
                                 &in_layout, in_fmt, (int)s.format->nSamplesPerSec, 0, nullptr);
    av_channel_layout_uninit(&out_layout);
    av_channel_layout_uninit(&in_layout);
    if (rc < 0 || !s.swr || swr_init(s.swr) < 0) {
        fprintf(stderr, "[audio] no pude preparar el conversor de formato\n");
        return false;
    }

    hr = s.client->Start();
    if (FAILED(hr)) {
        fprintf(stderr, "[audio] no pude arrancar la captura (0x%08lx)\n", (unsigned long)hr);
        return false;
    }

    s.ok = true;
    return true;
}

void AudioCapture::close_source(Source &s) {
    if (s.client && s.ok) s.client->Stop();
    if (s.capture) { s.capture->Release(); s.capture = nullptr; }
    if (s.client) { s.client->Release(); s.client = nullptr; }
    if (s.device) { s.device->Release(); s.device = nullptr; }
    if (s.format) { CoTaskMemFree(s.format); s.format = nullptr; }
    if (s.swr) { swr_free(&s.swr); s.swr = nullptr; }
    if (s.event) { CloseHandle(s.event); s.event = nullptr; }
    s.fifo.clear();
    s.zero_in.clear();
    s.ok = false;
    s.primed = false;
    s.compensating = false;
    s.last_drift_us = 0;
}

// ---------------------------------------------------------------------------
// open / close
// ---------------------------------------------------------------------------
bool AudioCapture::open(int sample_rate, int channels, const std::string &sys_id,
                        const std::string &mic_id, bool enable_mic, float mic_gain) {
    if (running_.load()) return true;
    sample_rate_ = sample_rate > 0 ? sample_rate : 48000;
    channels_ = channels > 0 ? channels : 2;
    mic_gain_ = mic_gain;

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr)) {
        com_inited_ = true;
    } else if (hr != RPC_E_CHANGED_MODE) {
        fprintf(stderr, "[audio] no pude iniciar COM (0x%08lx)\n", (unsigned long)hr);
        return false;
    }

    if (!open_source(sys_, eRender, true, sys_id)) {
        fprintf(stderr, "[audio] no pude capturar el audio del sistema\n");
        close_source(sys_);
        if (com_inited_) { CoUninitialize(); com_inited_ = false; }
        return false;
    }

    if (enable_mic && open_source(mic_, eCapture, false, mic_id)) {
        has_mic_ = true;
    } else {
        if (enable_mic) fprintf(stderr, "[audio] no hay microfono disponible; se graba solo el sistema\n");
        close_source(mic_);
    }

    stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    {
        std::lock_guard<std::mutex> lk(mutex_);
        out_fifo_.clear();
    }
    start_us_ = now_us();
    produced_frames_ = 0;
    stop_.store(false);
    running_.store(true);
    thread_ = std::thread(&AudioCapture::thread_main, this);

    fprintf(stderr, "[audio] %d Hz, %d canales (%s)\n", sample_rate_, channels_,
            has_mic_ ? "sistema + microfono" : "solo sistema");
    return true;
}

void AudioCapture::close() {
    stop_.store(true);
    if (stop_event_) SetEvent(stop_event_);
    if (thread_.joinable()) thread_.join();
    if (stop_event_) { CloseHandle(stop_event_); stop_event_ = nullptr; }

    close_source(sys_);
    close_source(mic_);

    running_.store(false);
    has_mic_ = false;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        out_fifo_.clear();
    }

    if (com_inited_) { CoUninitialize(); com_inited_ = false; }
}

// ---------------------------------------------------------------------------
// Sacar un paquete de un endpoint y dejarlo convertido en su FIFO
// ---------------------------------------------------------------------------
void AudioCapture::push_frames(Source &s, const uint8_t *data, UINT32 frames, bool silent) {
    if (frames == 0) return;
    int in_ch = s.format->nChannels;
    int bps = s.format->wBitsPerSample / 8;
    size_t in_size = (size_t)frames * in_ch * bps;

    const uint8_t *in = data;
    if (silent) {
        if (s.zero_in.size() < in_size) s.zero_in.resize(in_size);
        memset(s.zero_in.data(), 0, in_size);
        in = s.zero_in.data();
    }

    int out_max = (int)av_rescale_rnd(frames, sample_rate_, s.format->nSamplesPerSec,
                                      AV_ROUND_UP) + 64;
    size_t old = s.fifo.size();
    s.fifo.resize(old + (size_t)out_max * channels_);

    uint8_t *outp = reinterpret_cast<uint8_t *>(s.fifo.data() + old);
    int got = swr_convert(s.swr, &outp, out_max, &in, (int)frames);
    s.fifo.resize(old + (size_t)(got > 0 ? got : 0) * channels_);

    // Tope: la correccion de deriva mantiene el FIFO cerca del margen; si aun
    // asi se pasa de 1 segundo (algo raro paso), lo recortamos al margen.
    size_t max_frames = (size_t)sample_rate_;
    size_t keep = (size_t)(kLatencyUs * sample_rate_ / 1000000);
    if (s.fifo.size() > max_frames * channels_)
        s.fifo.erase(s.fifo.begin(), s.fifo.end() - keep * channels_);
}

// ---------------------------------------------------------------------------
// Deriva: el reloj de la placa de sonido nunca va exactamente al mismo ritmo
// que el de la PC. Si el FIFO de una fuente se aleja del margen, le pedimos a
// swresample que estire o encoja ese audio (como mucho 0,5%, inaudible) hasta
// volver al margen. Asi no hay cortes ni el audio se va corriendo con las horas.
// ---------------------------------------------------------------------------
void AudioCapture::adjust_drift(Source &s) {
    if (!s.ok || !s.primed || !s.swr) return;
    int64_t now = now_us();
    if (now - s.last_drift_us < 250000) return; // revisamos cada 250 ms
    s.last_drift_us = now;

    int64_t level = (int64_t)(s.fifo.size() / channels_);
    int64_t target = kLatencyUs * sample_rate_ / 1000000;
    int64_t err = level - target;
    int64_t dead = sample_rate_ / 50; // +-20 ms: dentro de eso no tocamos nada
    int64_t max_delta = sample_rate_ / 200;

    int delta = 0;
    if (err > dead || err < -dead) {
        int64_t d = -err;
        if (d > max_delta) d = max_delta;
        if (d < -max_delta) d = -max_delta;
        delta = (int)d;
    }
    if (delta == 0 && !s.compensating) return;
    // delta muestras de mas (o de menos) repartidas en el proximo segundo.
    if (swr_set_compensation(s.swr, delta, sample_rate_) >= 0) s.compensating = (delta != 0);
}

void AudioCapture::drain(Source &s) {
    if (!s.ok) return;
    UINT32 frames = 0;
    while (s.capture->GetNextPacketSize(&frames) == S_OK && frames != 0) {
        BYTE *data = nullptr;
        UINT32 n = 0;
        DWORD flags = 0;
        if (FAILED(s.capture->GetBuffer(&data, &n, &flags, nullptr, nullptr))) break;
        bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
        push_frames(s, data, n, silent);
        s.capture->ReleaseBuffer(n);
    }
}

// ---------------------------------------------------------------------------
// Producir la mezcla a ritmo de tiempo real (con mutex_ tomado)
// ---------------------------------------------------------------------------
void AudioCapture::produce_locked() {
    // La mezcla va kLatencyUs por detras del reloj: la muestra N se produce
    // en start_us_ + kLatencyUs + N/sr, pero contiene lo capturado en
    // start_us_ + N/sr (cada FIFO de entrada guarda justo ese margen).
    int64_t target = (now_us() - start_us_ - kLatencyUs) * sample_rate_ / 1000000;
    int64_t todo = target - produced_frames_;
    if (todo <= 0) return;

    const int64_t kChunk = sample_rate_ / 10; // hasta 100 ms por vuelta
    const size_t latency_frames = (size_t)(kLatencyUs * sample_rate_ / 1000000);

    while (todo > 0) {
        int block = (int)(todo < kChunk ? todo : kChunk);
        size_t out_off = out_fifo_.size();
        out_fifo_.resize(out_off + (size_t)block * channels_);
        int16_t *dst = out_fifo_.data() + out_off;

        // Una fuente que se quedo sin datos espera a juntar el margen entero
        // antes de volver a sonar; si no, cada paquete que llega 1 ms tarde
        // dejaria un hueco de silencio (chasquido) y un desfase.
        if (!sys_.primed && sys_.fifo.size() / channels_ >= latency_frames) sys_.primed = true;
        if (has_mic_ && !mic_.primed && mic_.fifo.size() / channels_ >= latency_frames)
            mic_.primed = true;

        int sys_frames = sys_.primed ? (int)(sys_.fifo.size() / channels_) : 0;
        int mic_frames = (has_mic_ && mic_.primed) ? (int)(mic_.fifo.size() / channels_) : 0;
        const float mic_gain = mic_gain_;

        for (int f = 0; f < block; f++) {
            for (int c = 0; c < channels_; c++) {
                int a = (f < sys_frames) ? sys_.fifo[(size_t)f * channels_ + c] : 0;
                int b = (f < mic_frames && has_mic_) ? mic_.fifo[(size_t)f * channels_ + c] : 0;
                if (mic_gain != 1.0f && b != 0)
                    b = (int)(b * mic_gain + (b > 0 ? 0.5f : -0.5f));
                int v = a + b;
                if (v > 32767) v = 32767;
                if (v < -32768) v = -32768;
                dst[(size_t)f * channels_ + c] = (int16_t)v;
            }
        }

        int consume = block < sys_frames ? block : sys_frames;
        if (consume > 0)
            sys_.fifo.erase(sys_.fifo.begin(), sys_.fifo.begin() + (size_t)consume * channels_);
        if (has_mic_) {
            consume = block < mic_frames ? block : mic_frames;
            if (consume > 0)
                mic_.fifo.erase(mic_.fifo.begin(),
                                mic_.fifo.begin() + (size_t)consume * channels_);
        }

        // Si alguna fuente no alcanzo para todo el bloque (por ejemplo, el
        // sistema dejo de sonar y Windows corto los paquetes), vuelve a juntar
        // margen.
        if (sys_.primed && sys_frames < block) sys_.primed = false;
        if (has_mic_ && mic_.primed && mic_frames < block) mic_.primed = false;

        produced_frames_ += block;
        todo -= block;
    }

    adjust_drift(sys_);
    if (has_mic_) adjust_drift(mic_);

    // Si nadie lee, no dejamos crecer la salida mas de 5 segundos.
    size_t max_out = (size_t)sample_rate_ * 5;
    if (out_fifo_.size() > max_out * channels_)
        out_fifo_.erase(out_fifo_.begin(), out_fifo_.end() - max_out * channels_);
}

// ---------------------------------------------------------------------------
// Hilo de captura
// ---------------------------------------------------------------------------
void AudioCapture::thread_main() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    HANDLE events[3];
    int n = 0;
    events[n++] = stop_event_;
    if (sys_.ok) events[n++] = sys_.event;
    if (mic_.ok) events[n++] = mic_.event;

    while (!stop_.load()) {
        DWORD r = WaitForMultipleObjects((DWORD)n, events, FALSE, 5);
        if (r == WAIT_FAILED) break;
        if (stop_.load()) break;

        drain(sys_);
        if (mic_.ok) drain(mic_);

        std::lock_guard<std::mutex> lk(mutex_);
        produce_locked();
    }

    CoUninitialize();
}

// ---------------------------------------------------------------------------
// Lectura desde el grabador
// ---------------------------------------------------------------------------
int AudioCapture::available_frames() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return (int)(out_fifo_.size() / channels_);
}

int AudioCapture::read(int16_t *dst, int max_frames, int64_t *first_index) {
    if (max_frames <= 0) return 0;
    std::lock_guard<std::mutex> lk(mutex_);
    int avail = (int)(out_fifo_.size() / channels_);
    // Lo que queda en out_fifo_ son las ultimas 'avail' muestras producidas.
    if (first_index) *first_index = produced_frames_ - avail;
    int n = avail < max_frames ? avail : max_frames;
    if (n <= 0) return 0;
    memcpy(dst, out_fifo_.data(), (size_t)n * channels_ * sizeof(int16_t));
    out_fifo_.erase(out_fifo_.begin(), out_fifo_.begin() + (size_t)n * channels_);
    return n;
}
