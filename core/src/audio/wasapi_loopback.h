// wasapi_loopback — C-callable shim around WASAPI loopback capture.
//
// Pulls the audio that Windows would be playing through the default
// render endpoint (the "what apps mix down to speakers" stream) into an
// internal ring buffer, ready to be consumed by an audio encoder on
// another thread.
//
// Internal format is always 32-bit float interleaved (matching Windows'
// audio engine output). Two reader APIs are exposed:
//   - wasapi_loopback_read_f32 returns the float samples as-is. Used by
//     the LDAC path so the full bit depth survives all the way to
//     ldacBT_encode (whose LDACBT_SMPL_FMT_F32 input mode takes float).
//   - wasapi_loopback_read returns int16 samples, doing the float→s16
//     truncation on the fly. Kept for backwards compatibility with the
//     M3.x SBC test tools, which were written before the float path
//     existed.
//
// All functions are thread-safe except where noted; init/start/stop
// should be called from a single owning thread (typically `main`).
// `read*` and `available*` can be called from any thread (typically the
// BTstack run loop thread).

#ifndef WIN_LDAC_WASAPI_LOOPBACK_H
#define WIN_LDAC_WASAPI_LOOPBACK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialise WASAPI capture on the default render endpoint in loopback
// mode. Doesn't start the capture thread yet; just queries the device
// format. Returns 0 on success, negative on failure. On success the
// output args are populated with the system mixer's format details.
//
//   *out_sample_rate : one of {44100, 48000, 88200, 96000}; anything
//                      else returns -2 (LDAC doesn't support it).
//   *out_channels    : usually 2 (stereo).
//
// Failure codes:
//   -1 : generic COM / device error
//   -2 : sample rate not supported by LDAC
//   -3 : sample format not IEEE float or s16
int  wasapi_loopback_init(int* out_sample_rate, int* out_channels);

// Read the device's nominal bit depth from the Windows endpoint
// property store (PKEY_AudioEngine_DeviceFormat). This is the value
// the user picked in Sound Properties → Advanced → "Default Format"
// (16, 24, or 32). May differ from what wasapi_loopback_init reports,
// since the engine internally is always 32-bit float regardless.
// Used for display purposes only — the actual capture path is float.
//
// Returns the bit depth (16/24/32) on success, 0 on failure. Safe to
// call any time after wasapi_loopback_init.
int  wasapi_loopback_device_bit_depth(void);

// Start the capture thread. After this returns, samples accumulate in
// the internal ring buffer until you stop the module. Returns 0 on
// success.
int  wasapi_loopback_start(void);

// Stop the capture thread and free all resources. Safe to call even if
// start failed.
void wasapi_loopback_stop(void);

// Read interleaved float32 stereo PCM samples (L R L R ...).
// `num_samples` is the number of float values, not the number of
// frames. Returns the number actually read (may be less than
// requested). Non-blocking.
size_t wasapi_loopback_read_f32(float* out, size_t num_samples);

// Read interleaved int16 stereo PCM samples. Same semantics as
// wasapi_loopback_read_f32 but with a float→s16 truncation step. Kept
// for SBC test tools.
size_t wasapi_loopback_read(int16_t* out, size_t num_samples);

// Current fill level (interleaved float sample count), for diagnostics.
size_t wasapi_loopback_available(void);

// Register a callback fired (on the capture thread) when WASAPI
// reports the device is gone, which typically means the user changed
// Sound Properties (sample-rate / bit-depth) or unplugged the device.
// The caller should ask the engine to rebuild WASAPI + the LDAC
// pipeline. Optional; default behaviour is to keep retrying.
typedef void (*wasapi_loopback_invalidated_fn)(void);
void wasapi_loopback_set_invalidated_callback(wasapi_loopback_invalidated_fn fn);

#ifdef __cplusplus
}
#endif

#endif  // WIN_LDAC_WASAPI_LOOPBACK_H
