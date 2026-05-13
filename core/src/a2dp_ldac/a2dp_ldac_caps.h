// a2dp_ldac_caps — LDAC vendor codec capability/configuration bytes.
//
// AVDTP Capability/Configuration payload layout for LDAC (per LDAC
// Specification of Bluetooth A2DP Rev.2.0.1 and AOSP
// a2dp_vendor_ldac_constants.h):
//
//   offset 0..3  Vendor ID  = 0x0000012D (Sony, little-endian)
//   offset 4..5  Codec ID   = 0x00AA      (LDAC, little-endian)
//   offset 6     Sampling-frequency bitmap (capability) /
//                single bit (configuration)
//   offset 7     Channel-mode bitmap (capability) /
//                single bit (configuration)
//
// In M4 these constants and helpers were inlined in a2dp_ldac_test.c.
// Pulled out here so M5+ (and future GUI status display) can share them.

#ifndef WIN_LDAC_A2DP_LDAC_CAPS_H
#define WIN_LDAC_A2DP_LDAC_CAPS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define A2DP_LDAC_VENDOR_ID  0x0000012Du  // Sony Corporation
#define A2DP_LDAC_CODEC_ID   0x00AAu      // LDAC

#define A2DP_LDAC_CAPS_LEN   8u

// Sampling-frequency support bits (offset 6).
#define A2DP_LDAC_SF_44100   0x20u
#define A2DP_LDAC_SF_48000   0x10u
#define A2DP_LDAC_SF_88200   0x08u
#define A2DP_LDAC_SF_96000   0x04u
#define A2DP_LDAC_SF_ALL     (A2DP_LDAC_SF_44100 | A2DP_LDAC_SF_48000 | \
                              A2DP_LDAC_SF_88200 | A2DP_LDAC_SF_96000)

// Channel-mode support bits (offset 7).
#define A2DP_LDAC_CM_MONO    0x04u
#define A2DP_LDAC_CM_DUAL    0x02u
#define A2DP_LDAC_CM_STEREO  0x01u

// Map a sampling-frequency bit to its Hz value (0 if unrecognised).
int  a2dp_ldac_sf_bit_to_hz(uint8_t sf_bit);

// Map a Hz value to its sampling-frequency bit (0 if unsupported).
uint8_t a2dp_ldac_hz_to_sf_bit(int hz);

// Human-readable strings for log lines.
const char* a2dp_ldac_sf_label(uint8_t sf_bit);
const char* a2dp_ldac_cm_label(uint8_t cm_bit);

// Return true iff `info` is a valid 8-byte LDAC vendor codec payload
// (right vendor + codec IDs, length >= 8).
bool a2dp_ldac_is_ldac_payload(const uint8_t* info, uint16_t info_len);

// Fill `out` (must have room for 8 bytes) with an LDAC capability
// announcement: vendor/codec IDs followed by the given bitmaps.
void a2dp_ldac_build_capability(uint8_t* out,
                                uint8_t sf_bitmap,
                                uint8_t cm_bitmap);

// Fill `out` (must have room for 8 bytes) with an LDAC SET_CONFIGURATION
// payload: vendor/codec IDs followed by single sample-rate and channel
// mode bits.
void a2dp_ldac_build_config(uint8_t* out,
                            uint8_t sf_bit,
                            uint8_t cm_bit);

// Pick the highest sample-rate bit in `bitmap` (96k > 88.2k > 48k > 44.1k).
// Returns 0 if `bitmap` is empty.
uint8_t a2dp_ldac_pick_best_sample_rate(uint8_t bitmap);

// Pick the closest sample-rate bit to `preferred_hz` from `bitmap`. Used
// to match the WASAPI mixer rate so we don't have to resample. If
// `preferred_hz` is supported by `bitmap`, returns its bit; otherwise
// falls back to a2dp_ldac_pick_best_sample_rate.
uint8_t a2dp_ldac_pick_sample_rate_for_hz(uint8_t bitmap, int preferred_hz);

// Pick the best channel mode (STEREO > DUAL > MONO).
uint8_t a2dp_ldac_pick_best_channel_mode(uint8_t bitmap);

#ifdef __cplusplus
}
#endif

#endif  // WIN_LDAC_A2DP_LDAC_CAPS_H
