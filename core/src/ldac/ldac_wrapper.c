// ldac_wrapper — see ldac_wrapper.h.

#include "ldac/ldac_wrapper.h"

#include <stdlib.h>
#include <string.h>

#include <ldacBT.h>
#include <ldacBT_abr.h>

struct ldac_wrapper {
    HANDLE_LDAC_BT  h;
    HANDLE_LDAC_ABR abr;
    int             mtu;
    int             eqmid;
    int             cm_libldac;
    int             sample_rate_hz;
    int             last_error;
    bool            abr_enabled;
};

int ldac_wrapper_cm_bit_to_libldac(uint8_t cm_bit) {
    switch (cm_bit) {
        case 0x01: return LDACBT_CHANNEL_MODE_STEREO;
        case 0x02: return LDACBT_CHANNEL_MODE_DUAL_CHANNEL;
        case 0x04: return LDACBT_CHANNEL_MODE_MONO;
        default:   return -1;
    }
}

int ldac_wrapper_sf_bit_to_hz(uint8_t sf_bit) {
    switch (sf_bit) {
        case LDACBT_SAMPLING_FREQ_044100: return 44100;
        case LDACBT_SAMPLING_FREQ_048000: return 48000;
        case LDACBT_SAMPLING_FREQ_088200: return 88200;
        case LDACBT_SAMPLING_FREQ_096000: return 96000;
        default:                           return 0;
    }
}

int ldac_wrapper_frames_per_encode(void) {
    return LDACBT_ENC_LSU;  // 128
}

// Per ldacBT.h §"Bit rates": HQ/SQ/MQ give 990/660/330 kbps at 48/96 kHz
// and 909/606/303 kbps at 44.1/88.2 kHz. We just hard-code that table
// since libldac doesn't expose it programmatically.
int ldac_wrapper_nominal_kbps(int eqmid, int sample_rate_hz) {
    bool is_x441_family = (sample_rate_hz == 44100 || sample_rate_hz == 88200);
    switch (eqmid) {
        case LDACBT_EQMID_HQ: return is_x441_family ? 909 : 990;
        case LDACBT_EQMID_SQ: return is_x441_family ? 606 : 660;
        case LDACBT_EQMID_MQ: return is_x441_family ? 303 : 330;
        default:              return 0;
    }
}

ldac_wrapper_t* ldac_wrapper_create(int mtu, int eqmid,
                                    int channel_mode_libldac,
                                    int sample_rate_hz) {
    if (mtu < 679) return NULL;  // libldac contract
    ldac_wrapper_t* w = (ldac_wrapper_t*)calloc(1, sizeof(*w));
    if (!w) return NULL;

    w->h = ldacBT_get_handle();
    if (!w->h) {
        free(w);
        return NULL;
    }

    int rc = ldacBT_init_handle_encode(
        w->h, mtu, eqmid,
        channel_mode_libldac,
        LDACBT_SMPL_FMT_F32,
        sample_rate_hz);
    if (rc != 0) {
        w->last_error = ldacBT_get_error_code(w->h);
        ldacBT_free_handle(w->h);
        free(w);
        return NULL;
    }

    w->mtu            = mtu;
    w->eqmid          = eqmid;
    w->cm_libldac     = channel_mode_libldac;
    w->sample_rate_hz = sample_rate_hz;
    return w;
}

void ldac_wrapper_destroy(ldac_wrapper_t* w) {
    if (!w) return;
    if (w->abr) ldac_ABR_free_handle(w->abr);
    if (w->h)   ldacBT_free_handle(w->h);
    free(w);
}

int ldac_wrapper_encode(ldac_wrapper_t* w,
                        const float* pcm,
                        uint8_t* out_buf, size_t out_buf_cap,
                        size_t* out_bytes_written,
                        int* out_frame_count) {
    if (out_bytes_written) *out_bytes_written = 0;
    if (out_frame_count)   *out_frame_count   = 0;
    if (!w || !w->h || !out_buf) return -1;

    int pcm_used = 0, stream_sz = 0, frame_num = 0;
    int rc = ldacBT_encode(
        w->h,
        (void*)pcm,         // libldac treats NULL as flush
        &pcm_used,
        out_buf,
        &stream_sz,
        &frame_num);
    if (rc != 0) {
        w->last_error = ldacBT_get_error_code(w->h);
        return -1;
    }
    if ((size_t)stream_sz > out_buf_cap) {
        // shouldn't happen — libldac caps frame size by the MTU we gave it.
        w->last_error = LDACBT_ERR_FATAL;
        return -1;
    }
    if (out_bytes_written) *out_bytes_written = (size_t)stream_sz;
    if (out_frame_count)   *out_frame_count   = frame_num;
    return 0;
}

int ldac_wrapper_set_eqmid(ldac_wrapper_t* w, int eqmid) {
    if (!w || !w->h) return -1;
    int rc = ldacBT_set_eqmid(w->h, eqmid);
    if (rc == 0) w->eqmid = eqmid;
    return rc;
}

int ldac_wrapper_abr_enable(ldac_wrapper_t* w, unsigned int interval_ms) {
    if (!w || !w->h) return -1;
    if (w->abr) return 0;  // already enabled
    w->abr = ldac_ABR_get_handle();
    if (!w->abr) return -1;
    int rc = ldac_ABR_Init(w->abr, interval_ms);
    if (rc != 0) {
        ldac_ABR_free_handle(w->abr);
        w->abr = NULL;
        return -1;
    }
    w->abr_enabled = true;
    return 0;
}

void ldac_wrapper_abr_proc(ldac_wrapper_t* w, unsigned int txq_depth) {
    if (!w || !w->h || !w->abr || !w->abr_enabled) return;
    // flagEnable=1 → ABR is allowed to call ldacBT_alter_eqmid_priority.
    int new_eqmid = ldac_ABR_Proc(w->h, w->abr, txq_depth, /*flagEnable=*/1);
    if (new_eqmid >= 0) {
        w->eqmid = new_eqmid;
    }
}

void ldac_wrapper_abr_disable(ldac_wrapper_t* w) {
    if (!w) return;
    if (w->abr) {
        ldac_ABR_free_handle(w->abr);
        w->abr = NULL;
    }
    w->abr_enabled = false;
}

bool ldac_wrapper_abr_is_enabled(const ldac_wrapper_t* w) {
    return w ? w->abr_enabled : false;
}

int ldac_wrapper_current_eqmid(const ldac_wrapper_t* w) {
    if (!w || !w->h) return -1;
    // ldacBT_get_eqmid reflects ABR-driven changes too.
    return ldacBT_get_eqmid(w->h);
}

int ldac_wrapper_last_error(const ldac_wrapper_t* w) {
    return w ? w->last_error : 0;
}

int ldac_wrapper_current_bitrate_kbps(const ldac_wrapper_t* w) {
    if (!w || !w->h) return 0;
    return ldacBT_get_bitrate(w->h);
}
