// wasapi_loopback — capture Windows render-side audio into a ring buffer.
//
// Single producer (the capture thread we start), single consumer (the
// caller of read()). A mutex-protected ring buffer is more than fast
// enough at 96 kHz * 2 ch * 4 byte/float = 768 kB/s — atomics give
// ~no benefit at this rate and complicate the code, so we keep it
// boring.
//
// Internal format change (M7): the ring buffer now stores float32.
// The LDAC path consumes it directly (LDACBT_SMPL_FMT_F32), which
// avoids the float→s16 truncation that used to cap the pipeline at
// 16-bit precision regardless of the user's Hi-Res sources. The s16
// reader still works for the SBC-era tools, doing the truncation only
// at the read boundary so it doesn't pollute the new path.
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
//   - AUDCLNT_E_DEVICE_INVALIDATED can come back from GetBuffer when
//     the user changes Sound Properties or unplugs the device. We
//     surface it via an optional callback so the engine can rebuild
//     the WASAPI session at the new format.

#include "wasapi_loopback.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

// INITGUID + initguid.h causes PROPERTYKEY constants declared in
// functiondiscoverykeys_devpkey.h (and the IID_* / KSDATAFORMAT_SUBTYPE_*
// GUIDs from the COM headers) to be locally defined here instead of
// only forward-declared. Without this we'd need to link propsys.lib.
#define INITGUID
#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <ksmedia.h>            // KSDATAFORMAT_SUBTYPE_*
#include <functiondiscoverykeys_devpkey.h>  // PKEY_AudioEngine_DeviceFormat

#pragma comment(lib, "ole32.lib")

namespace {

// ─── Boring mutex-protected ring buffer (float32) ────────────────────
class PcmRing {
 public:
    explicit PcmRing(size_t capacity_samples) : buf_(capacity_samples) {}

    size_t write(const float* p, size_t n) {
        std::lock_guard<std::mutex> lk(m_);
        size_t can = (std::min)(n, buf_.size() - count_);
        for (size_t i = 0; i < can; ++i) {
            buf_[tail_] = p[i];
            tail_ = (tail_ + 1) % buf_.size();
        }
        count_ += can;
        return can;
    }
    size_t write_silence(size_t n) {
        std::lock_guard<std::mutex> lk(m_);
        size_t can = (std::min)(n, buf_.size() - count_);
        for (size_t i = 0; i < can; ++i) {
            buf_[tail_] = 0.0f;
            tail_ = (tail_ + 1) % buf_.size();
        }
        count_ += can;
        return can;
    }
    size_t read_f32(float* p, size_t n) {
        std::lock_guard<std::mutex> lk(m_);
        size_t can = (std::min)(n, count_);
        for (size_t i = 0; i < can; ++i) {
            p[i] = buf_[head_];
            head_ = (head_ + 1) % buf_.size();
        }
        count_ -= can;
        return can;
    }
    // float→s16 truncation done on the read boundary for SBC tools.
    size_t read_s16(int16_t* p, size_t n) {
        std::lock_guard<std::mutex> lk(m_);
        size_t can = (std::min)(n, count_);
        for (size_t i = 0; i < can; ++i) {
            float v = buf_[head_];
            if (v >  1.0f) v =  1.0f;
            if (v < -1.0f) v = -1.0f;
            p[i] = static_cast<int16_t>(v * 32767.0f);
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
    std::vector<float> buf_;
    size_t             head_  = 0;
    size_t             tail_  = 0;
    size_t             count_ = 0;
    std::mutex         m_;
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
    int                  device_bit_depth = 0;
    wasapi_loopback_invalidated_fn invalidated_cb = nullptr;
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

// Read the device's nominal "Default Format" from the property store.
// This is what Windows Sound Properties → Advanced shows in the dropdown
// (16-bit / 24-bit / 32-bit, paired with a sample rate). The engine's
// internal mix format is always float32, so GetMixFormat doesn't tell
// us this; we have to ask the endpoint directly.
//
// Returns 16/24/32 on success, 0 on failure.
int read_endpoint_bit_depth(IMMDevice* device) {
    if (!device) return 0;
    IPropertyStore* props = nullptr;
    HRESULT hr = device->OpenPropertyStore(STGM_READ, &props);
    if (FAILED(hr) || !props) return 0;
    int result = 0;
    PROPVARIANT pv;
    PropVariantInit(&pv);
    hr = props->GetValue(PKEY_AudioEngine_DeviceFormat, &pv);
    if (SUCCEEDED(hr) && pv.vt == VT_BLOB && pv.blob.cbSize >= sizeof(WAVEFORMATEX)) {
        const WAVEFORMATEX* wf =
            reinterpret_cast<const WAVEFORMATEX*>(pv.blob.pBlobData);
        result = static_cast<int>(wf->wBitsPerSample);
    }
    PropVariantClear(&pv);
    props->Release();
    return result;
}

// Convert one interleaved frame of int16 PCM to interleaved float32.
// Used when WASAPI's mix format is the (rare) 16-bit PCM case.
inline void s16_to_float(const int16_t* in, float* out, size_t n_samples) {
    constexpr float k = 1.0f / 32768.0f;
    for (size_t i = 0; i < n_samples; ++i) {
        out[i] = static_cast<float>(in[i]) * k;
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
    std::vector<float> scratch(8192);  // grows if needed

    while (!g->stop_flag.load(std::memory_order_relaxed)) {
        DWORD wr = WaitForSingleObject(g->event, 200);
        if (wr == WAIT_TIMEOUT) continue;
        if (wr != WAIT_OBJECT_0) break;

        UINT32 packet_size = 0;
        while (true) {
            hr = g->capture->GetNextPacketSize(&packet_size);
            if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
                std::fprintf(stderr, "[wasapi] device invalidated — "
                                     "notifying engine\n");
                if (g->invalidated_cb) g->invalidated_cb();
                goto thread_exit;
            }
            if (FAILED(hr) || packet_size == 0) break;

            BYTE*  data   = nullptr;
            UINT32 frames = 0;
            DWORD  flags  = 0;
            hr = g->capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
            if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
                std::fprintf(stderr, "[wasapi] device invalidated — "
                                     "notifying engine\n");
                if (g->invalidated_cb) g->invalidated_cb();
                goto thread_exit;
            }
            if (FAILED(hr)) break;

            size_t samples = static_cast<size_t>(frames) * channels;

            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                g->ring->write_silence(samples);
            } else if (g->is_float) {
                // Zero-copy float32 path.
                g->ring->write(reinterpret_cast<const float*>(data), samples);
            } else if (g->is_s16) {
                if (scratch.size() < samples) scratch.resize(samples);
                s16_to_float(reinterpret_cast<const int16_t*>(data),
                             scratch.data(), samples);
                g->ring->write(scratch.data(), samples);
            }
            g->capture->ReleaseBuffer(frames);
        }
    }

thread_exit:
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

    s->sample_rate      = static_cast<int>(s->mix_format->nSamplesPerSec);
    s->channels         = static_cast<int>(s->mix_format->nChannels);
    s->device_bit_depth = read_endpoint_bit_depth(s->device);

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
    // jitter on either thread. At 96k stereo float32 that's ~384 KB —
    // still tiny.
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

extern "C" int wasapi_loopback_device_bit_depth(void) {
    return g ? g->device_bit_depth : 0;
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

extern "C" size_t wasapi_loopback_read_f32(float* out, size_t n) {
    if (!g || !g->ring) return 0;
    return g->ring->read_f32(out, n);
}

extern "C" size_t wasapi_loopback_read(int16_t* out, size_t n) {
    if (!g || !g->ring) return 0;
    return g->ring->read_s16(out, n);
}

extern "C" size_t wasapi_loopback_available(void) {
    if (!g || !g->ring) return 0;
    return g->ring->available();
}

extern "C" void wasapi_loopback_set_invalidated_callback(
        wasapi_loopback_invalidated_fn fn) {
    if (!g) return;
    g->invalidated_cb = fn;
}
