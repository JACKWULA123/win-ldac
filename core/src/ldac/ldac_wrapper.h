// ldac_wrapper — thin C shim around libldac's encoder.
//
// libldac's API is already C, so this module exists mainly to (a) bundle
// up the handle lifecycle in one place, (b) translate channel mode bits
// from the A2DP capability bitmap into LDAC's channel_mode constant,
// (c) expose a small encode helper that hides "pump until libldac stops
// emitting bytes" loops, (d) wrap the ABR helpers from libldac/abr/.
//
// Input format is float32 (LDACBT_SMPL_FMT_F32). WASAPI's audio engine
// is already float32 internally, so this preserves the source bit depth
// all the way to the encoder — important for Hi-Res 24-bit material.
//
// Threading: A single ldac_wrapper_t belongs to exactly one thread. We
// drive it from the BTstack run loop thread.

#ifndef WIN_LDAC_LDAC_WRAPPER_H
#define WIN_LDAC_LDAC_WRAPPER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ldac_wrapper ldac_wrapper_t;

// Map an LDAC A2DP channel-mode capability bit (0x01 STEREO / 0x02 DUAL /
// 0x04 MONO) to libldac's channel_mode constant. Returns -1 if the bit
// isn't recognised.
int ldac_wrapper_cm_bit_to_libldac(uint8_t cm_bit);

// Map an LDAC A2DP sampling-frequency bit (0x20/0x10/0x08/0x04 etc.) to
// the corresponding Hz value (44100/48000/88200/96000). Returns 0 if the
// bit isn't recognised.
int ldac_wrapper_sf_bit_to_hz(uint8_t sf_bit);

// Return the number of stereo PCM frames the encoder consumes per
// ldacBT_encode call. Constant 128 per libldac contract — exposed here
// so callers don't have to include ldacBT.h.
int ldac_wrapper_frames_per_encode(void);

// Nominal LDAC bitrate (kbps) for a given EQMID at a given sample rate.
// Useful for GUI display ("we're targeting 990 kbps right now").
// Returns 0 for unrecognised inputs.
int ldac_wrapper_nominal_kbps(int eqmid, int sample_rate_hz);

// Create an encoder. `mtu` is the AVDTP MTU (per libldac docs the MUST
// be >= 679). `eqmid` is one of the LDACBT_EQMID_* values; pass 0 for
// HQ. `channel_mode_libldac` and `sample_rate_hz` come from the helpers
// above.
//
// On failure returns NULL.
ldac_wrapper_t* ldac_wrapper_create(int mtu, int eqmid,
                                    int channel_mode_libldac,
                                    int sample_rate_hz);

void ldac_wrapper_destroy(ldac_wrapper_t* w);

// Encode one chunk (exactly `ldac_wrapper_frames_per_encode()` stereo
// frames of float32 PCM, interleaved L R L R ...). May emit zero, one,
// or several LDAC transport frames into `out_buf`.
//
//   pcm                Pointer to 128 * 2 float samples (1024 bytes).
//                      Pass NULL to flush (during shutdown).
//   out_buf            Destination buffer for the LDAC transport frame
//                      sequence. Must be at least LDACBT_MAX_NBYTES.
//   out_buf_cap        Capacity of out_buf in bytes.
//   out_bytes_written  Number of bytes written into out_buf (0 if libldac
//                      is still buffering input).
//   out_frame_count    Number of LDAC transport frames emitted (0 or 1
//                      in practice).
//
// Returns 0 on success, -1 on libldac error (including fatal); on -1 the
// caller should not feed the encoder more data without re-creating it.
int ldac_wrapper_encode(ldac_wrapper_t* w,
                        const float* pcm,
                        uint8_t* out_buf, size_t out_buf_cap,
                        size_t* out_bytes_written,
                        int* out_frame_count);

// Change the encoder's quality mode mid-stream. `eqmid` is one of
// LDACBT_EQMID_* (0=HQ, 1=SQ, 2=MQ). For ABR mode, see the helpers
// below — don't call this directly.
int ldac_wrapper_set_eqmid(ldac_wrapper_t* w, int eqmid);

// Adaptive bitrate (ABR) integration. libldac's abr lib watches the
// transmit-queue depth and nudges the EQMID up/down to match link
// capacity. The engine feeds it a packet-count-style queue depth
// every Δms and ABR decides if/when to call ldacBT_alter_eqmid_priority.
//
// Lifecycle:
//   ldac_wrapper_abr_enable(w, interval_ms)
//      → ABR handle is allocated, ldac_ABR_Init called with the period
//        at which the engine will call ldac_wrapper_abr_proc.
//   ldac_wrapper_abr_proc(w, txq_depth_packets)
//      → call this every `interval_ms`; passes the queue depth on to
//        ldac_ABR_Proc. ABR will alter EQMID when warranted.
//   ldac_wrapper_abr_disable(w)
//      → frees the ABR handle and locks EQMID back to whatever was
//        configured at create-time.
int  ldac_wrapper_abr_enable(ldac_wrapper_t* w, unsigned int interval_ms);
void ldac_wrapper_abr_proc(ldac_wrapper_t* w, unsigned int txq_depth);
void ldac_wrapper_abr_disable(ldac_wrapper_t* w);
bool ldac_wrapper_abr_is_enabled(const ldac_wrapper_t* w);

// Returns the current EQMID (0=HQ, 1=SQ, 2=MQ) for status display.
int ldac_wrapper_current_eqmid(const ldac_wrapper_t* w);

// Most recent libldac error code (or 0). For diagnostic logging only.
int ldac_wrapper_last_error(const ldac_wrapper_t* w);

// Currently-configured bitrate, in kbps. Useful for status reporting.
// Returns 0 if not initialised.
int ldac_wrapper_current_bitrate_kbps(const ldac_wrapper_t* w);

#ifdef __cplusplus
}
#endif

#endif  // WIN_LDAC_LDAC_WRAPPER_H
