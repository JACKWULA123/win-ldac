// wasapi_loopback — capture Windows render-side audio into a ring buffer.
//
// Single producer (the capture thread we start), single consumer (the
// caller of read()). A mutex-protected ring buffer is more than fast
// enough at 48 kHz * 2 ch = 192 kB/s — atomics give ~no benefit at this
// rate and complicate the code, so we keep it boring.
//
// COM details:
//   - We init COM as MTA inside the capture thread.
//   - GetMixFormat() returns the device's *shared-mode* mix format,
//     which is what loopback always uses. We can't change it; we
//     adapt to it.
//   - Loopback is the same IAudioClient API as input, just with the
//     AUDCLNT_STREAMFLAGS_LOOPBACK flag and the render endpoint.
//   - When no app is playing, WASAPI signals the event handle but
//     returns AUDCLNT_BUFFERFLAGS_SILENT — we still push the silence
//     into the ring buffer to keep the timing tight.

#include "wasapi_loopback.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <ksmedia.h>  // KSDATAFORMAT_SUBTYPE_*

#pragma comment(lib, "ole32.lib")

namespace {

// ─── A boring mutex-protected ring buffer ────────────────────────────
class PcmRing {
 public:
    explicit PcmRing(size_t capacity_samples) : buf_(capacity_samples) {}

    size_t write(const int16_t* p, size_t n) {
        std::lock_guard<std::mutex> lk(m_);
        size_t can = (std::min)(n, buf_.size() - count_);
        for (size_t i = 0; i < can; ++i) {
            buf_[tail_] = p[i];
            tail_ = (tail_ + 1) % buf_.size();
        }
        count_ += can;
        return can;
    }
    // Write `n` zero samples. Used for AUDCLNT_BUFFERFLAGS_SILENT packets.
    size_t write_silence(size_t n) {
        std::lock_guard<std::mutex> lk(m_);
        size_t can = (std::min)(n, buf_.size() - count_);
        for (size_t i = 0; i < can; ++i) {
            buf_[tail_] = 0;
            tail_ = (tail_ + 1) % buf_.size();
        }
        count_ += can;
        return can;
    }
    size_t read(int16_t* p, size_t n) {
        std::lock_guard<std::mutex> lk(m_);
        size_t can = (std::min)(n, count_);
        for (size_t i = 0; i < can; ++i) {
            p[i] = buf_[head_];
            head_ = (head_ + 1) % buf_.size();
        }
        count_ -= can;
        return can;
    }
    size_t available() {
        std::lock_guard<std::mutex> lk(m_);
        return count_;
    }

 private:
    std::vector<int16_t> buf_;
    size_t               head_  = 0;
    size_t               tail_  = 0;
    size_t               count_ = 0;
    std::mutex           m_;
};

// ─── Module state ────────────────────────────────────────────────────
struct State {
    IMMDeviceEnumerator* enumerator    = nullptr;
    IMMDevice*           device        = nullptr;
    IAudioClient*        audio_client  = nullptr;
    IAudioCaptureClient* capture       = nullptr;
    WAVEFORMATEX*        mix_format    = nullptr;
    HANDLE               event         = nullptr;
    PcmRing*             ring          = nullptr;
    std::thread          capture_thread;
    std::atomic<bool>    stop_flag{false};
    bool                 is_float      = false;
    bool                 is_s16        = false;
    int                  sample_rate   = 0;
    int                  channels      = 0;
};

State* g = nullptr;

#define SAFE_RELEASE(p) do { if (p) { (p)->Release(); (p) = nullptr; } } while (0)

bool detect_format(WAVEFORMATEX* wf, bool* is_float, bool* is_s16) {
    *is_float = false;
    *is_s16   = false;
    if (wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        *is_float = true;
        return wf->wBitsPerSample == 32;
    }
    if (wf->wFormatTag == WAVE_FORMAT_PCM) {
        *is_s16 = (wf->wBitsPerSample == 16);
        return *is_s16;
    }
    if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const WAVEFORMATEXTENSIBLE* ext =
            reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf);
        if (IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {
            *is_float = (wf->wBitsPerSample == 32);
            return *is_float;
        }
        if (IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_PCM)) {
            *is_s16 = (wf->wBitsPerSample == 16);
            return *is_s16;
        }
    }
    return false;
}

// Convert a single interleaved frame of IEEE float to interleaved s16.
inline void float_to_s16(const float* in, int16_t* out, size_t n_samples) {
    for (size_t i = 0; i < n_samples; ++i) {
        float v = in[i];
        if (v >  1.0f) v =  1.0f;
        if (v < -1.0f) v = -1.0f;
        out[i] = static_cast<int16_t>(v * 32767.0f);
    }
}

void capture_thread_main() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        std::fprintf(stderr, "[wasapi] CoInitializeEx failed: 0x%lx\n", hr);
        return;
    }

    hr = g->audio_client->Start();
    if (FAILED(hr)) {
        std::fprintf(stderr, "[wasapi] IAudioClient::Start failed: 0x%lx\n", hr);
        CoUninitialize();
        return;
    }

    const int channels = g->channels;
    std::vector<int16_t> scratch(8192);  // grows if needed

    while (!g->stop_flag.load(std::memory_order_relaxed)) {
        DWORD wr = WaitForSingleObject(g->event, 200);
        if (wr == WAIT_TIMEOUT) continue;
        if (wr != WAIT_OBJECT_0) break;

        UINT32 packet_size = 0;
        while (true) {
            hr = g->capture->GetNextPacketSize(&packet_size);
            if (FAILED(hr) || packet_size == 0) break;

            BYTE*  data   = nullptr;
            UINT32 frames = 0;
            DWORD  flags  = 0;
            hr = g->capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
            if (FAILED(hr)) break;

            size_t samples = static_cast<size_t>(frames) * channels;

            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                g->ring->write_silence(samples);
            } else {
                if (scratch.size() < samples) scratch.resize(samples);
                if (g->is_float) {
                    float_to_s16(reinterpret_cast<const float*>(data),
                                 scratch.data(), samples);
                    g->ring->write(scratch.data(), samples);
                } else if (g->is_s16) {
                    g->ring->write(reinterpret_cast<const int16_t*>(data),
                                   samples);
                }
            }
            g->capture->ReleaseBuffer(frames);
        }
    }

    g->audio_client->Stop();
    CoUninitialize();
}

}  // namespace

// ─── Public API ──────────────────────────────────────────────────────
extern "C" int wasapi_loopback_init(int* out_sample_rate, int* out_channels) {
    if (g) return 0;  // already initialised

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool com_inited = SUCCEEDED(hr);
    // RPC_E_CHANGED_MODE means COM was already inited as MTA — that's fine
    // for the device-enumeration calls we do here.

    State* s = new State();

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                          CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                          reinterpret_cast<void**>(&s->enumerator));
    if (FAILED(hr)) {
        std::fprintf(stderr,
            "[wasapi] CoCreateInstance(MMDeviceEnumerator) failed: 0x%lx\n", hr);
        goto fail;
    }

    hr = s->enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &s->device);
    if (FAILED(hr)) {
        std::fprintf(stderr,
            "[wasapi] GetDefaultAudioEndpoint failed: 0x%lx\n", hr);
        goto fail;
    }

    hr = s->device->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                             nullptr,
                             reinterpret_cast<void**>(&s->audio_client));
    if (FAILED(hr)) {
        std::fprintf(stderr,
            "[wasapi] IMMDevice::Activate(IAudioClient) failed: 0x%lx\n", hr);
        goto fail;
    }

    hr = s->audio_client->GetMixFormat(&s->mix_format);
    if (FAILED(hr)) {
        std::fprintf(stderr, "[wasapi] GetMixFormat failed: 0x%lx\n", hr);
        goto fail;
    }

    if (!detect_format(s->mix_format, &s->is_float, &s->is_s16)) {
        std::fprintf(stderr,
            "[wasapi] Unsupported mixer format: tag=0x%x bits=%u\n",
            s->mix_format->wFormatTag, s->mix_format->wBitsPerSample);
        std::fprintf(stderr,
            "         Set Windows audio to 16-bit or 32-bit float, 44.1/48 kHz.\n");
        delete s;
        if (com_inited) CoUninitialize();
        return -3;
    }

    s->sample_rate = static_cast<int>(s->mix_format->nSamplesPerSec);
    s->channels    = static_cast<int>(s->mix_format->nChannels);

    // LDAC supports {44.1, 48, 88.2, 96} kHz; anything else needs a
    // resampler, which we don't implement. SBC tools only handle the
    // lower two rates — at 88.2/96 kHz the SBC negotiation will fail
    // downstream, but wasapi_loopback itself stays codec-agnostic.
    if (s->sample_rate != 44100 && s->sample_rate != 48000 &&
        s->sample_rate != 88200 && s->sample_rate != 96000) {
        std::fprintf(stderr,
            "[wasapi] System mixer is %d Hz; LDAC only supports\n"
            "         44.1 / 48 / 88.2 / 96 kHz. Set Windows → Sound →\n"
            "         Properties → Advanced to one of those rates.\n",
            s->sample_rate);
        delete s;
        if (com_inited) CoUninitialize();
        return -2;
    }
    if (s->channels != 2) {
        std::fprintf(stderr,
            "[wasapi] System mixer reports %d channels; only stereo supported.\n",
            s->channels);
        delete s;
        if (com_inited) CoUninitialize();
        return -1;
    }

    {
        // 200 ms buffer. Loopback wants share mode + event callback.
        const REFERENCE_TIME buffer_duration = 200 * 10000;  // 100ns units
        hr = s->audio_client->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
            buffer_duration, 0, s->mix_format, nullptr);
        if (FAILED(hr)) {
            std::fprintf(stderr,
                "[wasapi] IAudioClient::Initialize failed: 0x%lx\n", hr);
            goto fail;
        }

        s->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!s->event) {
            std::fprintf(stderr, "[wasapi] CreateEvent failed\n");
            goto fail;
        }
        hr = s->audio_client->SetEventHandle(s->event);
        if (FAILED(hr)) {
            std::fprintf(stderr,
                "[wasapi] SetEventHandle failed: 0x%lx\n", hr);
            goto fail;
        }

        hr = s->audio_client->GetService(
            __uuidof(IAudioCaptureClient),
            reinterpret_cast<void**>(&s->capture));
        if (FAILED(hr)) {
            std::fprintf(stderr,
                "[wasapi] GetService(IAudioCaptureClient) failed: 0x%lx\n", hr);
            goto fail;
        }
    }

    // ~0.5 s of stereo PCM as a generous safety margin against scheduling
    // jitter on either thread. At 48k stereo s16 that's ~96 KB.
    s->ring = new PcmRing(static_cast<size_t>(s->sample_rate)
                          * s->channels / 2);

    g = s;
    if (out_sample_rate) *out_sample_rate = s->sample_rate;
    if (out_channels)    *out_channels    = s->channels;
    // Don't CoUninitialize here — the audio interfaces hold refs that
    // need the apartment to stay alive until stop.
    return 0;

fail:
    if (s->capture)      s->capture->Release();
    if (s->audio_client) s->audio_client->Release();
    if (s->device)       s->device->Release();
    if (s->enumerator)   s->enumerator->Release();
    if (s->mix_format)   CoTaskMemFree(s->mix_format);
    if (s->event)        CloseHandle(s->event);
    delete s;
    if (com_inited) CoUninitialize();
    return -1;
}

extern "C" int wasapi_loopback_start(void) {
    if (!g) return -1;
    g->stop_flag.store(false);
    g->capture_thread = std::thread(capture_thread_main);
    return 0;
}

extern "C" void wasapi_loopback_stop(void) {
    if (!g) return;
    g->stop_flag.store(true);
    if (g->capture_thread.joinable()) {
        // Nudge the event so the thread exits the wait promptly.
        if (g->event) SetEvent(g->event);
        g->capture_thread.join();
    }
    SAFE_RELEASE(g->capture);
    SAFE_RELEASE(g->audio_client);
    SAFE_RELEASE(g->device);
    SAFE_RELEASE(g->enumerator);
    if (g->mix_format) { CoTaskMemFree(g->mix_format); g->mix_format = nullptr; }
    if (g->event)      { CloseHandle(g->event);        g->event      = nullptr; }
    delete g->ring;
    delete g;
    g = nullptr;
    CoUninitialize();
}

extern "C" size_t wasapi_loopback_read(int16_t* out, size_t n) {
    if (!g || !g->ring) return 0;
    return g->ring->read(out, n);
}

extern "C" size_t wasapi_loopback_available(void) {
    if (!g || !g->ring) return 0;
    return g->ring->available();
}
