// ldac_encode_test — M1 acceptance test.
//
// Generates a synthetic stereo 48 kHz / 16-bit sine wave, encodes it with
// libldac at HQ quality, writes the raw ldac_transport_frame stream to
// out.ldac, and reports the effective bitrate.
//
// Acceptance: effective bitrate ≈ 990 kbps (HQ at 48 kHz target per
// ldacBT.h §Bit rates table). PASS if within ±2 %.

#include <ldacBT.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

constexpr int    SAMPLE_RATE   = 48000;
constexpr int    CHANNELS      = 2;
constexpr int    DURATION_SEC  = 10;
constexpr int    MTU           = 679;       // A2DP minimum (ldacBT.h §MTU)
constexpr double TARGET_KBPS   = 990.0;     // HQ @ 48 kHz
constexpr double TOLERANCE_PCT = 2.0;

// Generate stereo 16-bit signed PCM with a 440 Hz sine on L and 880 Hz on R.
// Interleaved layout L0 R0 L1 R1 ... as required by libldac.
std::vector<int16_t> make_sine_pcm(int sample_rate, int channels,
                                   int duration_sec) {
    const size_t frames = static_cast<size_t>(sample_rate) * duration_sec;
    std::vector<int16_t> pcm(frames * channels);
    const double two_pi = 6.28318530717958647692;
    for (size_t i = 0; i < frames; ++i) {
        double t = static_cast<double>(i) / sample_rate;
        double l = std::sin(two_pi * 440.0 * t) * 16000.0;
        double r = std::sin(two_pi * 880.0 * t) * 16000.0;
        pcm[i * 2 + 0] = static_cast<int16_t>(l);
        pcm[i * 2 + 1] = static_cast<int16_t>(r);
    }
    return pcm;
}

const char* err_class(int err) {
    if (LDACBT_FATAL(err)) return "FATAL";
    if (LDACBT_ERROR(err)) return "non-fatal";
    return "ok";
}

}  // namespace

int main(int argc, char** argv) {
    const std::string out_path = (argc > 1) ? argv[1] : "out.ldac";

    std::printf("LDAC encode test (M1)\n");
    std::printf("  sample rate : %d Hz\n", SAMPLE_RATE);
    std::printf("  channels    : %d\n", CHANNELS);
    std::printf("  duration    : %d s\n", DURATION_SEC);
    std::printf("  EQMID       : HQ (target %.0f kbps)\n", TARGET_KBPS);
    std::printf("  output      : %s\n\n", out_path.c_str());

    auto pcm = make_sine_pcm(SAMPLE_RATE, CHANNELS, DURATION_SEC);
    const size_t pcm_total_bytes = pcm.size() * sizeof(int16_t);

    HANDLE_LDAC_BT h = ldacBT_get_handle();
    if (!h) {
        std::fprintf(stderr, "ldacBT_get_handle() returned NULL\n");
        return 1;
    }

    int rc = ldacBT_init_handle_encode(
        h,
        MTU,
        LDACBT_EQMID_HQ,
        LDACBT_CHANNEL_MODE_STEREO,
        LDACBT_SMPL_FMT_S16,
        SAMPLE_RATE);
    if (rc != 0) {
        int e = ldacBT_get_error_code(h);
        std::fprintf(stderr, "init_handle_encode failed rc=%d err=0x%X (%s)\n",
                     rc, e, err_class(e));
        ldacBT_free_handle(h);
        return 1;
    }

    std::ofstream out(out_path, std::ios::binary);
    if (!out) {
        std::fprintf(stderr, "cannot open %s for writing\n", out_path.c_str());
        ldacBT_free_handle(h);
        return 1;
    }

    // Each encode call consumes exactly LDACBT_ENC_LSU (128) samples
    // per channel. For 48 kHz stereo S16 that's 128 * 2 * 2 = 512 bytes.
    const size_t bytes_per_call =
        static_cast<size_t>(LDACBT_ENC_LSU) * CHANNELS * sizeof(int16_t);

    // Encoder may emit up to MAX_NBYTES per call, but the API doc doesn't
    // promise a strict upper bound, so allocate generously.
    std::vector<uint8_t> obuf(LDACBT_MAX_NBYTES * 4);

    size_t in_offset      = 0;
    size_t out_total      = 0;
    int    frames_total   = 0;
    int    encode_calls   = 0;

    while (in_offset + bytes_per_call <= pcm_total_bytes) {
        int pcm_used = 0, stream_sz = 0, frame_num = 0;
        uint8_t* in_ptr =
            reinterpret_cast<uint8_t*>(pcm.data()) + in_offset;
        rc = ldacBT_encode(h, in_ptr, &pcm_used,
                           obuf.data(), &stream_sz, &frame_num);
        if (rc != 0) {
            int e = ldacBT_get_error_code(h);
            std::fprintf(stderr,
                         "encode failed at offset %zu: rc=%d err=0x%X (%s)\n",
                         in_offset, rc, e, err_class(e));
            break;
        }
        in_offset += pcm_used;
        if (stream_sz > 0) {
            out.write(reinterpret_cast<const char*>(obuf.data()), stream_sz);
            out_total += stream_sz;
            frames_total += frame_num;
        }
        ++encode_calls;
    }

    // Flush remaining frames by passing NULL pcm (per ldacBT.h).
    for (int i = 0; i < 8; ++i) {
        int pcm_used = 0, stream_sz = 0, frame_num = 0;
        rc = ldacBT_encode(h, nullptr, &pcm_used,
                           obuf.data(), &stream_sz, &frame_num);
        if (rc != 0) break;
        if (stream_sz <= 0) break;
        out.write(reinterpret_cast<const char*>(obuf.data()), stream_sz);
        out_total += stream_sz;
        frames_total += frame_num;
    }

    out.close();
    ldacBT_free_handle(h);

    const double duration_sec =
        static_cast<double>(in_offset / (CHANNELS * sizeof(int16_t)))
        / SAMPLE_RATE;
    const double bitrate_kbps =
        (static_cast<double>(out_total) * 8.0) / duration_sec / 1000.0;
    const double error_pct =
        std::fabs(bitrate_kbps - TARGET_KBPS) / TARGET_KBPS * 100.0;
    const bool pass = error_pct <= TOLERANCE_PCT;

    std::printf("results:\n");
    std::printf("  encode calls    : %d\n", encode_calls);
    std::printf("  pcm consumed    : %zu bytes (%.3f s)\n",
                in_offset, duration_sec);
    std::printf("  ldac stream     : %zu bytes\n", out_total);
    std::printf("  transport frames: %d\n", frames_total);
    std::printf("  bitrate         : %.2f kbps  (target %.0f, deviation %.2f%%)\n",
                bitrate_kbps, TARGET_KBPS, error_pct);
    std::printf("\n  %s\n",
                pass ? "[ PASS ]  M1 acceptance met"
                     : "[ FAIL ]  bitrate outside tolerance");

    return pass ? 0 : 2;
}
