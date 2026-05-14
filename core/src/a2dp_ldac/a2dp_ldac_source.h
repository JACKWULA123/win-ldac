// a2dp_ldac_source — streaming driver: PCM → libldac → AVDTP MEDIA PDU.
//
// Wraps the BTstack A2DP source flow control (`request_can_send_now` /
// `STREAMING_CAN_SEND_MEDIA_PACKET_NOW`) with a libldac encoder fed from
// an arbitrary PCM callback. The caller's job is to drive the BTstack
// signaling (connect, negotiate, start stream) and hand us:
//   - the negotiated a2dp_cid + local_seid
//   - the negotiated sample rate and channel mode
//   - a function pointer that supplies interleaved float32 stereo PCM
//
// Once `setup` + `start` are called, the source's internal timer fires
// every few ms, pulls PCM, encodes, packs an AVDTP media payload (1-byte
// LDAC media header + N LDAC transport frames), and asks BTstack to send
// it. M5 keeps things simple: one LDAC transport frame per AVDTP packet
// (no batching, no fragmentation). At HQ@48k that's ~330 byte frames
// inside an MTU of ≥ 679 — always fits.
//
// M7 additions:
//   - Float32 PCM end to end (no s16 truncation).
//   - Effective bitrate measurement: bytes successfully submitted to
//     BTstack over a 1-second sliding window. Tracks "what the link is
//     actually carrying" rather than the encoder's nominal target.
//   - TX-queue-depth proxy: outstanding-packet count (requested but not
//     yet sent by BTstack), suitable as input to ldac_ABR_Proc.
//   - Bitrate mode toggle: fixed-HQ vs adaptive.
//
// Threading: every function in this header must be called from the
// BTstack run loop thread.

#ifndef WIN_LDAC_A2DP_LDAC_SOURCE_H
#define WIN_LDAC_A2DP_LDAC_SOURCE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// PCM source callback: write up to `num_float_samples` interleaved float
// values (L R L R ... for stereo) into `out`. Return the count actually
// written; we treat the shortfall as underrun and pad with silence.
typedef size_t (*a2dp_ldac_source_pull_pcm_fn)(float* out,
                                               size_t num_float_samples);

typedef enum {
    A2DP_LDAC_BITRATE_FIXED_HQ = 0,  // EQMID locked at HQ
    A2DP_LDAC_BITRATE_ADAPTIVE,      // ldac_ABR_Proc drives EQMID
} a2dp_ldac_bitrate_mode_t;

// One-time setup. Creates the libldac encoder configured for the
// negotiated stream. `channel_mode_a2dp_bit` is the LDAC A2DP channel-
// mode bit (0x01/0x02/0x04 — see a2dp_ldac_caps.h). `mode` picks
// fixed-HQ vs adaptive. Returns 0 on success.
int  a2dp_ldac_source_setup(uint16_t a2dp_cid,
                            uint8_t  local_seid,
                            int      sample_rate_hz,
                            uint8_t  channel_mode_a2dp_bit,
                            a2dp_ldac_bitrate_mode_t mode,
                            a2dp_ldac_source_pull_pcm_fn pull_pcm);

// Start the audio timer. Call from A2DP_SUBEVENT_STREAM_STARTED.
void a2dp_ldac_source_start(void);

// Pause the audio timer and discard any half-built packet. Encoder
// state is preserved so streaming can resume with start().
void a2dp_ldac_source_stop(void);

// On A2DP_SUBEVENT_STREAMING_CAN_SEND_MEDIA_PACKET_NOW, hand off the
// staged AVDTP packet to BTstack and try to build the next one
// immediately.
void a2dp_ldac_source_on_can_send_now(void);

// Tear down the encoder. After this, setup() can be called again to
// reuse the module for a new stream.
void a2dp_ldac_source_teardown(void);

// Switch the bitrate mode without tearing the encoder down. May be
// called any time after setup. No-op if not initialised.
void a2dp_ldac_source_set_bitrate_mode(a2dp_ldac_bitrate_mode_t mode);

// ── Diagnostic accessors ───────────────────────────────────────────────
uint64_t a2dp_ldac_source_underrun_samples(void);
int      a2dp_ldac_source_negotiated_sample_rate(void);

// Encoder's *nominal* bitrate per the configured/current EQMID
// (990/660/330 at 48/96 kHz; 909/606/303 at 44.1/88.2 kHz).
int      a2dp_ldac_source_nominal_kbps(void);

// Effective bitrate: bytes successfully submitted to BTstack in the
// last ~1 second, in kbps. Closer to what the BT link is actually
// pushing through than the nominal value, because BTstack
// back-pressures us via the CAN_SEND_NOW signal when the radio link
// can't keep up. Updated lazily; readers may see slightly stale
// values.
int      a2dp_ldac_source_effective_kbps(void);

// Outstanding packet count: packets we asked BTstack to send for which
// we have NOT yet received the corresponding CAN_SEND_NOW
// notification. Equivalent to the BTstack TX queue depth from our
// perspective. Fed into ldac_ABR_Proc when bitrate mode = Adaptive.
int      a2dp_ldac_source_outstanding_packets(void);

// Currently-selected EQMID (0=HQ, 1=SQ, 2=MQ). In fixed mode this
// stays at the value passed to setup(); in adaptive mode ABR may
// change it.
int      a2dp_ldac_source_current_eqmid(void);

a2dp_ldac_bitrate_mode_t a2dp_ldac_source_current_bitrate_mode(void);

#ifdef __cplusplus
}
#endif

#endif  // WIN_LDAC_A2DP_LDAC_SOURCE_H
