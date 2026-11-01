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

static int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
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
bool AudioCapture::open_source(Source &s, EDataFlow flow, bool loopback) {
    IMMDeviceEnumerator *enumerator = nullptr;
    HRESULT hr = CoCreateInstance(kClsidMMDeviceEnumerator, nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator), (void **)&enumerator);
    if (FAILED(hr)) return false;

    hr = enumerator->GetDefaultAudioEndpoint(flow, eConsole, &s.device);
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
}

// ---------------------------------------------------------------------------
// open / close
// ---------------------------------------------------------------------------
bool AudioCapture::open(int sample_rate, int channels) {
    if (running_.load()) return true;
    sample_rate_ = sample_rate > 0 ? sample_rate : 48000;
    channels_ = channels > 0 ? channels : 2;

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr)) {
        com_inited_ = true;
    } else if (hr != RPC_E_CHANGED_MODE) {
        fprintf(stderr, "[audio] no pude iniciar COM (0x%08lx)\n", (unsigned long)hr);
        return false;
    }

    if (!open_source(sys_, eRender, true)) {
        fprintf(stderr, "[audio] no pude capturar el audio del sistema\n");
        close_source(sys_);
        if (com_inited_) { CoUninitialize(); com_inited_ = false; }
        return false;
    }

    if (open_source(mic_, eCapture, false)) {
        has_mic_ = true;
    } else {
        fprintf(stderr, "[audio] no hay microfono disponible; se graba solo el sistema\n");
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

    // Tope: si un endpoint se atrasa, no acumulamos mas de 2 segundos.
    size_t max_frames = (size_t)sample_rate_ * 2;
    if (s.fifo.size() > max_frames * channels_)
        s.fifo.erase(s.fifo.begin(), s.fifo.end() - max_frames * channels_);
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
    int64_t target = (now_us() - start_us_) * sample_rate_ / 1000000;
    int64_t todo = target - produced_frames_;
    if (todo <= 0) return;

    const int64_t kChunk = sample_rate_ / 10; // hasta 100 ms por vuelta

    while (todo > 0) {
        int block = (int)(todo < kChunk ? todo : kChunk);
        size_t out_off = out_fifo_.size();
        out_fifo_.resize(out_off + (size_t)block * channels_);
        int16_t *dst = out_fifo_.data() + out_off;

        int sys_frames = (int)(sys_.fifo.size() / channels_);
        int mic_frames = has_mic_ ? (int)(mic_.fifo.size() / channels_) : 0;

        for (int f = 0; f < block; f++) {
            for (int c = 0; c < channels_; c++) {
                int a = (f < sys_frames) ? sys_.fifo[(size_t)f * channels_ + c] : 0;
                int b = (f < mic_frames && has_mic_) ? mic_.fifo[(size_t)f * channels_ + c] : 0;
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

        produced_frames_ += block;
        todo -= block;
    }

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

int AudioCapture::read(int16_t *dst, int max_frames) {
    if (max_frames <= 0) return 0;
    std::lock_guard<std::mutex> lk(mutex_);
    int avail = (int)(out_fifo_.size() / channels_);
    int n = avail < max_frames ? avail : max_frames;
    if (n <= 0) return 0;
    memcpy(dst, out_fifo_.data(), (size_t)n * channels_ * sizeof(int16_t));
    out_fifo_.erase(out_fifo_.begin(), out_fifo_.begin() + (size_t)n * channels_);
    return n;
}
