// a2dp_ldac_caps — see a2dp_ldac_caps.h.

#include "a2dp_ldac/a2dp_ldac_caps.h"

int a2dp_ldac_sf_bit_to_hz(uint8_t sf_bit) {
    switch (sf_bit) {
        case A2DP_LDAC_SF_44100: return 44100;
        case A2DP_LDAC_SF_48000: return 48000;
        case A2DP_LDAC_SF_88200: return 88200;
        case A2DP_LDAC_SF_96000: return 96000;
        default:                 return 0;
    }
}

uint8_t a2dp_ldac_hz_to_sf_bit(int hz) {
    switch (hz) {
        case 44100: return A2DP_LDAC_SF_44100;
        case 48000: return A2DP_LDAC_SF_48000;
        case 88200: return A2DP_LDAC_SF_88200;
        case 96000: return A2DP_LDAC_SF_96000;
        default:    return 0;
    }
}

const char* a2dp_ldac_sf_label(uint8_t sf_bit) {
    switch (sf_bit) {
        case A2DP_LDAC_SF_44100: return "44100 Hz";
        case A2DP_LDAC_SF_48000: return "48000 Hz";
        case A2DP_LDAC_SF_88200: return "88200 Hz";
        case A2DP_LDAC_SF_96000: return "96000 Hz";
        default:                 return "(unknown)";
    }
}

const char* a2dp_ldac_cm_label(uint8_t cm_bit) {
    switch (cm_bit) {
        case A2DP_LDAC_CM_STEREO: return "STEREO";
        case A2DP_LDAC_CM_DUAL:   return "DUAL_CHANNEL";
        case A2DP_LDAC_CM_MONO:   return "MONO";
        default:                  return "(unknown)";
    }
}

bool a2dp_ldac_is_ldac_payload(const uint8_t* info, uint16_t info_len) {
    if (!info || info_len < A2DP_LDAC_CAPS_LEN) return false;
    uint32_t vendor =
        (uint32_t)info[0] |
        ((uint32_t)info[1] << 8) |
        ((uint32_t)info[2] << 16) |
        ((uint32_t)info[3] << 24);
    uint16_t codec = (uint16_t)info[4] | ((uint16_t)info[5] << 8);
    return vendor == A2DP_LDAC_VENDOR_ID && codec == A2DP_LDAC_CODEC_ID;
}

void a2dp_ldac_build_capability(uint8_t* out,
                                uint8_t sf_bitmap,
                                uint8_t cm_bitmap) {
    out[0] = 0x2D;  // vendor id LSB
    out[1] = 0x01;
    out[2] = 0x00;
    out[3] = 0x00;
    out[4] = 0xAA;  // codec id LSB
    out[5] = 0x00;
    out[6] = sf_bitmap;
    out[7] = cm_bitmap;
}

void a2dp_ldac_build_config(uint8_t* out,
                            uint8_t sf_bit,
                            uint8_t cm_bit) {
    a2dp_ldac_build_capability(out, sf_bit, cm_bit);
}

uint8_t a2dp_ldac_pick_best_sample_rate(uint8_t bitmap) {
    static const uint8_t prefer[] = {
        A2DP_LDAC_SF_96000, A2DP_LDAC_SF_88200,
        A2DP_LDAC_SF_48000, A2DP_LDAC_SF_44100,
    };
    for (size_t i = 0; i < sizeof(prefer); ++i) {
        if (bitmap & prefer[i]) return prefer[i];
    }
    return 0;
}

uint8_t a2dp_ldac_pick_sample_rate_for_hz(uint8_t bitmap, int preferred_hz) {
    uint8_t preferred_bit = a2dp_ldac_hz_to_sf_bit(preferred_hz);
    if (preferred_bit && (bitmap & preferred_bit)) return preferred_bit;
    return a2dp_ldac_pick_best_sample_rate(bitmap);
}

uint8_t a2dp_ldac_pick_best_channel_mode(uint8_t bitmap) {
    if (bitmap & A2DP_LDAC_CM_STEREO) return A2DP_LDAC_CM_STEREO;
    if (bitmap & A2DP_LDAC_CM_DUAL)   return A2DP_LDAC_CM_DUAL;
    if (bitmap & A2DP_LDAC_CM_MONO)   return A2DP_LDAC_CM_MONO;
    return 0;
}
