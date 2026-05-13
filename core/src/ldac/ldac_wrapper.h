// ldac_wrapper — thin C shim around libldac's encoder.
//
// libldac's API is already C, so this module exists mainly to (a) bundle
// up the handle lifecycle in one place, (b) translate channel mode bits
// from the A2DP capability bitmap into LDAC's channel_mode constant,
// (c) expose a small encode helper that hides "pump until libldac stops
// emitting bytes" loops.
//
// Threading: A single ldac_wrapper_t belongs to exactly one thread. We
// drive it from the BTstack run loop thread.

#ifndef WIN_LDAC_LDAC_WRAPPER_H
#define WIN_LDAC_LDAC_WRAPPER_H

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

// Create an encoder. `mtu` is the AVDTP MTU (per libldac docs the MUST
// be >= 679). `eqmid` is one of the LDACBT_EQMID_* values; pass 0 for
// HQ. `channel_mode_libldac` and `sample_rate_hz` come from the helpers
// above. PCM format is fixed at signed-16-bit little-endian interleaved
// (matches what wasapi_loopback feeds us).
//
// On failure returns NULL.
ldac_wrapper_t* ldac_wrapper_create(int mtu, int eqmid,
                                    int channel_mode_libldac,
                                    int sample_rate_hz);

void ldac_wrapper_destroy(ldac_wrapper_t* w);

// Encode one chunk (exactly `ldac_wrapper_frames_per_encode()` stereo
// frames of int16 PCM, interleaved L R L R ...). May emit zero, one, or
// several LDAC transport frames into `out_buf`.
//
//   pcm                Pointer to 128 * 2 int16 samples (512 bytes).
//                      Pass NULL to flush (during shutdown).
//   out_buf            Destination buffer for the LDAC transport frame
//                      sequence. Must be at least LDACBT_MAX_NBYTES *
//                      LDAC_MAX_FRAMES_PER_CALL bytes; in practice 1024
//                      is enough at MTU 679 because libldac never emits
//                      more than one frame per call after init.
//   out_buf_cap        Capacity of out_buf in bytes.
//   out_bytes_written  Number of bytes written into out_buf (0 if libldac
//                      is still buffering input).
//   out_frame_count    Number of LDAC transport frames emitted (0 or 1
//                      in practice).
//
// Returns 0 on success, -1 on libldac error (including fatal); on -1 the
// caller should not feed the encoder more data without re-creating it.
int ldac_wrapper_encode(ldac_wrapper_t* w,
                        const int16_t* pcm,
                        uint8_t* out_buf, size_t out_buf_cap,
                        size_t* out_bytes_written,
                        int* out_frame_count);

// Most recent libldac error code (or 0). For diagnostic logging only.
int ldac_wrapper_last_error(const ldac_wrapper_t* w);

// Currently-configured bitrate, in kbps. Useful for status reporting.
// Returns 0 if not initialised.
int ldac_wrapper_current_bitrate_kbps(const ldac_wrapper_t* w);

#ifdef __cplusplus
}
#endif

#endif  // WIN_LDAC_LDAC_WRAPPER_H
