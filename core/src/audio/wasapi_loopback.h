// wasapi_loopback — C-callable shim around WASAPI loopback capture.
//
// Pulls the audio that Windows would be playing through the default
// render endpoint (the "what apps mix down to speakers" stream) into an
// internal ring buffer, ready to be consumed by an audio encoder on
// another thread.
//
// All functions are thread-safe except where noted; init/start/stop
// should be called from a single owning thread (typically `main`).
// `read` and `available` can be called from any thread (typically the
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
//   *out_sample_rate : 44100 or 48000 normally; 88200/96000/etc. will
//                      return -2 ("unsupported rate for SBC/LDAC").
//   *out_channels    : usually 2 (stereo).
//
// Failure codes:
//   -1 : generic COM / device error
//   -2 : sample rate not supported (advise user to set 44.1k or 48k)
//   -3 : sample format not IEEE float or s16
int  wasapi_loopback_init(int* out_sample_rate, int* out_channels);

// Start the capture thread. After this returns, samples accumulate in
// the internal ring buffer until you stop the module. Returns 0 on
// success.
int  wasapi_loopback_start(void);

// Stop the capture thread and free all resources. Safe to call even if
// start failed.
void wasapi_loopback_stop(void);

// Read interleaved int16 stereo PCM samples (L R L R ...). `num_samples`
// is the number of int16 values, not the number of frames. Returns the
// number actually read (may be less than requested). Non-blocking.
size_t wasapi_loopback_read(int16_t* out, size_t num_samples);

// Current fill level (interleaved int16 sample count), for diagnostics.
size_t wasapi_loopback_available(void);

#ifdef __cplusplus
}
#endif

#endif  // WIN_LDAC_WASAPI_LOOPBACK_H
