// a2dp_ldac_source — streaming driver: PCM → libldac → AVDTP MEDIA PDU.
//
// Wraps the BTstack A2DP source flow control (`request_can_send_now` /
// `STREAMING_CAN_SEND_MEDIA_PACKET_NOW`) with a libldac encoder fed from
// an arbitrary PCM callback. The caller's job is to drive the BTstack
// signaling (connect, negotiate, start stream) and hand us:
//   - the negotiated a2dp_cid + local_seid
//   - the negotiated sample rate and channel mode
//   - a function pointer that supplies interleaved int16 stereo PCM
//
// Once `setup` + `start` are called, the source's internal timer fires
// every few ms, pulls PCM, encodes, packs an AVDTP media payload (1-byte
// LDAC media header + N LDAC transport frames), and asks BTstack to send
// it. M5 keeps things simple: one LDAC transport frame per AVDTP packet
// (no batching, no fragmentation). At HQ@48k that's ~330 byte frames
// inside an MTU of ≥ 679 — always fits.
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

// PCM source callback: write up to `num_int16_samples` interleaved int16
// values (L R L R ... for stereo) into `out`. Return the count actually
// written; we treat the shortfall as underrun and pad with silence.
typedef size_t (*a2dp_ldac_source_pull_pcm_fn)(int16_t* out,
                                               size_t num_int16_samples);

// One-time setup. Creates the libldac encoder configured for the
// negotiated stream. `channel_mode_a2dp_bit` is the LDAC A2DP channel-
// mode bit (0x01/0x02/0x04 — see a2dp_ldac_caps.h). `eqmid` is one of
// LDACBT_EQMID_* (pass 0 for HQ). Returns 0 on success.
int  a2dp_ldac_source_setup(uint16_t a2dp_cid,
                            uint8_t  local_seid,
                            int      sample_rate_hz,
                            uint8_t  channel_mode_a2dp_bit,
                            int      eqmid,
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

// Diagnostic accessors (safe to call any time after setup).
uint64_t a2dp_ldac_source_underrun_samples(void);
int      a2dp_ldac_source_current_bitrate_kbps(void);
int      a2dp_ldac_source_negotiated_sample_rate(void);

#ifdef __cplusplus
}
#endif

#endif  // WIN_LDAC_A2DP_LDAC_SOURCE_H
