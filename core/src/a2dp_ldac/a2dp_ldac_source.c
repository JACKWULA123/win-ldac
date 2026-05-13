// a2dp_ldac_source — see a2dp_ldac_source.h.
//
// Pacing strategy
// ---------------
// At HQ@48k, libldac emits one ~330-byte LDAC transport frame every
// 2.67 ms (375 fps). That's much faster than an audio timer reasonably
// runs (10 ms). To keep up we chain CAN_SEND requests: every time
// BTstack signals CAN_SEND_NOW, we send the packet we just built, then
// immediately try to build the next one and request CAN_SEND again. The
// timer's only job is to keep `samples_owed` topped up from elapsed
// real time so the encoder always has PCM to feed.
//
// M5 v1 puts exactly one LDAC transport frame in each AVDTP packet. The
// LDAC media payload header byte therefore always has frame_count=1 and
// all fragmentation flags clear. Batching multiple frames per packet is
// an easy follow-up if/when MTU > 2 * frame_size — left for later.

#include "a2dp_ldac/a2dp_ldac_source.h"

#include <stdio.h>
#include <string.h>

#include "btstack.h"

#include "a2dp_ldac/a2dp_ldac_caps.h"
#include "ldac/ldac_wrapper.h"

// ── Constants ──────────────────────────────────────────────────────────
#define AUDIO_TIMER_MS              5     // see "Pacing strategy" above
#define LDAC_PCM_FRAMES_PER_CALL    128   // libldac fixed input chunk
#define LDAC_CHANNELS               2     // M5 stereo only
#define LDAC_MEDIA_PAYLOAD_HDR_LEN  1     // F/S/L flags + frame count

// ── Module state ───────────────────────────────────────────────────────
typedef struct {
    bool                  initialised;
    uint16_t              a2dp_cid;
    uint8_t               local_seid;
    int                   sample_rate_hz;
    int                   samples_per_ldac_frame;  // 128 (<=48k) or 256 (>=88.2k)
    int                   max_payload_size;        // from BTstack
    ldac_wrapper_t*       enc;
    a2dp_ldac_source_pull_pcm_fn pull_pcm;

    // Audio-time accounting (elapsed-time → samples-still-to-encode).
    uint32_t              time_last_tick_ms;
    uint32_t              samples_owed;            // PCM frames per channel
    uint32_t              acc_missed_ppm;
    btstack_timer_source_t audio_timer;

    // Build buffer for the current outgoing AVDTP media payload:
    //   buf[0]      = LDAC media payload header (frame count + flags)
    //   buf[1..N]   = concatenated LDAC transport frames
    //
    // packet_byte_count is the body byte count (i.e. excludes buf[0]).
    uint8_t               packet_buf[2048];
    int                   packet_byte_count;
    int                   packet_frame_count;
    bool                  packet_pending;          // true after request_can_send_now

    bool                  streaming;
    uint32_t              rtp_timestamp;
    uint64_t              underrun_samples;
} state_t;
static state_t s;

// ── Forward decls ──────────────────────────────────────────────────────
static void audio_timer_handler(btstack_timer_source_t* ts);
static void pump_until_packet_ready(void);
static void send_current_packet(void);

// ── Helpers ────────────────────────────────────────────────────────────
static int samples_per_frame_for_rate(int hz) {
    // LDAC core frame size, in PCM samples per channel (see ldacBT.h
    // §"Sampling frequency and frame sample"). Matters for RTP timestamp
    // accounting on the receiver side.
    return (hz >= 88200) ? 256 : 128;
}

// ── Public API ─────────────────────────────────────────────────────────
int a2dp_ldac_source_setup(uint16_t a2dp_cid,
                           uint8_t  local_seid,
                           int      sample_rate_hz,
                           uint8_t  channel_mode_a2dp_bit,
                           int      eqmid,
                           a2dp_ldac_source_pull_pcm_fn pull_pcm) {
    if (s.initialised) {
        fprintf(stderr, "[a2dp_ldac_source] already initialised\n");
        return -1;
    }
    if (!pull_pcm) return -1;
    if (channel_mode_a2dp_bit != A2DP_LDAC_CM_STEREO) {
        // M5 keeps to stereo. Mono/dual will need a different PCM pull
        // path (channel count != 2) — straightforward, just not done.
        fprintf(stderr,
            "[a2dp_ldac_source] M5 only supports STEREO channel mode "
            "(got 0x%02x)\n", channel_mode_a2dp_bit);
        return -1;
    }
    int cm_libldac = ldac_wrapper_cm_bit_to_libldac(channel_mode_a2dp_bit);
    if (cm_libldac < 0) return -1;

    // BTstack tells us how many bytes will actually fit in one AVDTP
    // media payload after its own RTP/AVDTP overhead.
    int payload = a2dp_max_media_payload_size(a2dp_cid, local_seid);
    if (payload <= 0) {
        fprintf(stderr,
            "[a2dp_ldac_source] a2dp_max_media_payload_size returned %d\n",
            payload);
        return -1;
    }
    if ((size_t)payload > sizeof(s.packet_buf)) {
        fprintf(stderr,
            "[a2dp_ldac_source] payload %d > buffer %zu\n",
            payload, sizeof(s.packet_buf));
        return -1;
    }

    // libldac wants the AVDTP MTU. We give it the payload size minus
    // our 1-byte LDAC media header, so its frame size budget reflects
    // what we can actually fit per packet.
    int mtu_for_libldac = payload - LDAC_MEDIA_PAYLOAD_HDR_LEN;
    if (mtu_for_libldac < 679) {
        // libldac itself enforces >= 679; surfacing the error here
        // is more informative than letting it fail deep inside init.
        fprintf(stderr,
            "[a2dp_ldac_source] negotiated payload too small "
            "(%d, need >= 680)\n", payload);
        return -1;
    }

    ldac_wrapper_t* enc = ldac_wrapper_create(
        mtu_for_libldac, eqmid, cm_libldac, sample_rate_hz);
    if (!enc) {
        fprintf(stderr, "[a2dp_ldac_source] ldac_wrapper_create failed\n");
        return -1;
    }

    memset(&s, 0, sizeof(s));
    s.initialised            = true;
    s.a2dp_cid               = a2dp_cid;
    s.local_seid             = local_seid;
    s.sample_rate_hz         = sample_rate_hz;
    s.samples_per_ldac_frame = samples_per_frame_for_rate(sample_rate_hz);
    s.max_payload_size       = payload;
    s.enc                    = enc;
    s.pull_pcm               = pull_pcm;
    return 0;
}

void a2dp_ldac_source_start(void) {
    if (!s.initialised) return;
    s.streaming         = true;
    s.time_last_tick_ms = 0;
    s.samples_owed      = 0;
    s.acc_missed_ppm    = 0;
    s.packet_byte_count = 0;
    s.packet_frame_count = 0;
    s.packet_pending    = false;
    s.underrun_samples  = 0;

    btstack_run_loop_remove_timer(&s.audio_timer);
    btstack_run_loop_set_timer_handler(&s.audio_timer, &audio_timer_handler);
    btstack_run_loop_set_timer(&s.audio_timer, AUDIO_TIMER_MS);
    btstack_run_loop_add_timer(&s.audio_timer);
}

void a2dp_ldac_source_stop(void) {
    if (!s.initialised) return;
    s.streaming         = false;
    s.samples_owed      = 0;
    s.acc_missed_ppm    = 0;
    s.packet_byte_count = 0;
    s.packet_frame_count = 0;
    s.packet_pending    = false;
    btstack_run_loop_remove_timer(&s.audio_timer);
}

void a2dp_ldac_source_teardown(void) {
    if (!s.initialised) return;
    a2dp_ldac_source_stop();
    if (s.enc) {
        ldac_wrapper_destroy(s.enc);
        s.enc = NULL;
    }
    memset(&s, 0, sizeof(s));
}

void a2dp_ldac_source_on_can_send_now(void) {
    if (!s.initialised || !s.streaming) return;
    if (s.packet_frame_count == 0) {
        s.packet_pending = false;
        return;
    }
    send_current_packet();
    // After the send, try to immediately build the next one if there's
    // enough PCM buffered. This is what keeps us in sync at high
    // bitrates (HQ@48k = 375 packets/sec, way too fast for the timer
    // alone).
    pump_until_packet_ready();
}

uint64_t a2dp_ldac_source_underrun_samples(void) {
    return s.underrun_samples;
}

int a2dp_ldac_source_current_bitrate_kbps(void) {
    return s.enc ? ldac_wrapper_current_bitrate_kbps(s.enc) : 0;
}

int a2dp_ldac_source_negotiated_sample_rate(void) {
    return s.sample_rate_hz;
}

// ── Internals ──────────────────────────────────────────────────────────
static void send_current_packet(void) {
    // LDAC media payload header (1 byte):
    //   bit 7 = F (fragmented),  bit 6 = S (start),  bit 5 = L (last),
    //   bits 4..0 = frame count (up to 31).
    // M5 always sends non-fragmented packets → flags clear, only the
    // frame count is set.
    s.packet_buf[0] = (uint8_t)(s.packet_frame_count & 0x1F);

    int total_len = LDAC_MEDIA_PAYLOAD_HDR_LEN + s.packet_byte_count;
    uint8_t status = a2dp_source_stream_send_media_payload_rtp(
        s.a2dp_cid, s.local_seid,
        /*marker*/ 0,
        s.rtp_timestamp,
        s.packet_buf, (uint16_t)total_len);
    if (status != ERROR_CODE_SUCCESS) {
        // Non-fatal: BTstack may briefly reject sends during retransmit
        // storms etc. The packet is dropped; the encoder marches on.
        fprintf(stderr,
            "[a2dp_ldac_source] send_media_payload_rtp status=0x%02x\n",
            status);
    }
    s.rtp_timestamp     += (uint32_t)s.packet_frame_count
                           * (uint32_t)s.samples_per_ldac_frame;
    s.packet_byte_count = 0;
    s.packet_frame_count = 0;
    s.packet_pending    = false;
}

// Encode + accumulate until the build buffer contains at least one
// transport frame, then request CAN_SEND. Returns silently if there's
// not enough buffered PCM or a request is already in flight.
static void pump_until_packet_ready(void) {
    if (!s.streaming || s.packet_pending) return;

    while (s.samples_owed >= (uint32_t)LDAC_PCM_FRAMES_PER_CALL) {
        // Pull exactly one libldac chunk's worth of interleaved s16 PCM.
        int16_t pcm[LDAC_PCM_FRAMES_PER_CALL * LDAC_CHANNELS];
        size_t requested = LDAC_PCM_FRAMES_PER_CALL * LDAC_CHANNELS;
        size_t got       = s.pull_pcm(pcm, requested);
        if (got < requested) {
            memset(pcm + got, 0, (requested - got) * sizeof(int16_t));
            s.underrun_samples += (requested - got);
        }

        // Encode straight into the body of the build buffer, after the
        // 1-byte LDAC media payload header.
        uint8_t* dst = s.packet_buf + LDAC_MEDIA_PAYLOAD_HDR_LEN
                       + s.packet_byte_count;
        size_t dst_cap = sizeof(s.packet_buf)
                         - LDAC_MEDIA_PAYLOAD_HDR_LEN
                         - (size_t)s.packet_byte_count;
        size_t out_bytes = 0;
        int    frame_num = 0;
        int rc = ldac_wrapper_encode(s.enc, pcm,
                                     dst, dst_cap,
                                     &out_bytes, &frame_num);
        if (rc != 0) {
            fprintf(stderr,
                "[a2dp_ldac_source] ldac_wrapper_encode failed, "
                "last_err=0x%x\n",
                ldac_wrapper_last_error(s.enc));
            // Encoder is sour; give up streaming until next start().
            a2dp_ldac_source_stop();
            return;
        }
        s.samples_owed -= LDAC_PCM_FRAMES_PER_CALL;

        if (out_bytes == 0) {
            // libldac is still buffering input (happens at 88.2/96 kHz
            // where 2 encode calls produce 1 transport frame).
            continue;
        }

        s.packet_byte_count  += (int)out_bytes;
        s.packet_frame_count += frame_num;

        // M5 keeps it at exactly one frame per AVDTP packet (see file
        // header). As soon as we have a frame, request a send.
        s.packet_pending = true;
        a2dp_source_stream_endpoint_request_can_send_now(
            s.a2dp_cid, s.local_seid);
        return;
    }
}

static void audio_timer_handler(btstack_timer_source_t* ts) {
    btstack_run_loop_set_timer(ts, AUDIO_TIMER_MS);
    btstack_run_loop_add_timer(ts);
    if (!s.streaming) return;

    uint32_t now = btstack_run_loop_get_time_ms();
    uint32_t period_ms;
    if (s.time_last_tick_ms == 0) {
        // First tick: assume one timer period elapsed. Avoids a huge
        // spike on startup that would over-credit samples_owed.
        period_ms = AUDIO_TIMER_MS;
    } else {
        period_ms = now - s.time_last_tick_ms;
    }
    s.time_last_tick_ms = now;

    // Credit PCM-frame ticks based on elapsed real time. The acc_missed
    // accumulator carries the sub-millisecond remainder so the long-run
    // rate matches sample_rate_hz exactly.
    uint32_t whole = (period_ms * (uint32_t)s.sample_rate_hz) / 1000u;
    s.acc_missed_ppm += (period_ms * (uint32_t)s.sample_rate_hz) % 1000u;
    while (s.acc_missed_ppm >= 1000u) {
        whole++;
        s.acc_missed_ppm -= 1000u;
    }
    s.samples_owed += whole;

    pump_until_packet_ready();
}
