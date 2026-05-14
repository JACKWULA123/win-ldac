// engine — see engine.h.
//
// All of the BTstack / A2DP / WASAPI plumbing that used to live inline
// in a2dp_ldac_persistent_test.c now lives here, so the same code can
// host either the CLI test tool or the upcoming Dear ImGui GUI.

#include "engine/engine.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "btstack.h"
#include "btstack_run_loop_windows.h"
#include "hci_transport_usb.h"
#include "btstack_chipset_realtek.h"

#include "audio/wasapi_loopback.h"
#include "a2dp_ldac/a2dp_ldac_caps.h"
#include "a2dp_ldac/a2dp_ldac_source.h"
#include "app/reconnect_supervisor.h"
#include "bt/link_key_db_file.h"

// ── Constants ──────────────────────────────────────────────────────────
#define CLASS_OF_DEVICE         0x200408
#define RTL8761BU_CANONICAL_PID 0x8771

// ── Helpers ────────────────────────────────────────────────────────────
static bool resolve_firmware_folder(char* out, size_t out_sz) {
    char exe[MAX_PATH];
    if (!GetModuleFileNameA(NULL, exe, MAX_PATH)) return false;
    char* sep = strrchr(exe, '\\');
    if (!sep) return false;
    *sep = '\0';
    char base[MAX_PATH], candidate[MAX_PATH];
    btstack_strcpy(base, sizeof(base), exe);
    for (int i = 0; i < 5; ++i) {
        btstack_snprintf_assert_complete(candidate, sizeof(candidate),
                                         "%s\\firmware", base);
        DWORD attr = GetFileAttributesA(candidate);
        if (attr != INVALID_FILE_ATTRIBUTES &&
            (attr & FILE_ATTRIBUTE_DIRECTORY)) {
            btstack_strcpy(out, out_sz, candidate);
            return true;
        }
        char* p = strrchr(base, '\\');
        if (!p) break;
        *p = '\0';
    }
    return false;
}

// ── Module state ───────────────────────────────────────────────────────
typedef struct {
    bd_addr_t                target;
    char                     local_name[64];
    uint32_t                 reconnect_interval_ms;
    engine_bitrate_mode_t    bitrate_mode;
    a2dp_ldac_bitrate_mode_t a2dp_bitrate_mode; // mirror of bitrate_mode

    // Per-link transient state (rebuilt every reconnect).
    uint16_t                 a2dp_cid;
    uint8_t                  local_seid;
    uint8_t                  remote_seid;
    bool                     link_up;
    bool                     stream_opened;
    bool                     ldac_capability_seen;
    bool                     config_applied;
    bool                     streaming_started;

    // Negotiation cache — preserved across reconnects so that when
    // BTstack skips OTHER_CAPABILITY on a quick reconnect (the cid
    // 0x0002 corner case from M6.2) we can still build the encoder
    // using last-known parameters.
    uint8_t                  set_config_payload[A2DP_LDAC_CAPS_LEN];
    int                      negotiated_sr_hz;
    uint8_t                  negotiated_cm_bit;

    // Static caps payload (we always advertise full LDAC, stereo).
    uint8_t                  ldac_caps[A2DP_LDAC_CAPS_LEN];
    uint8_t                  ldac_config_buf[A2DP_LDAC_CAPS_LEN];

    // SDP records (must stay alive for the program lifetime).
    uint8_t                  sdp_a2dp_source_buf[150];
    uint8_t                  sdp_device_id_buf[100];

    int                      wasapi_sample_rate;
    int                      wasapi_channels;

    btstack_packet_callback_registration_t hci_cb_reg;

    engine_state_t           state;
    engine_log_fn            log_fn;

    // Cross-thread snapshot lock.
    CRITICAL_SECTION         snapshot_lock;
    bool                     snapshot_lock_inited;

    // Atomic-ish stop flag (set from any thread).
    volatile LONG            stop_requested;
} engine_t;

static engine_t e;

// ── Forward decls ──────────────────────────────────────────────────────
static void hci_packet_handler(uint8_t pt, uint16_t ch,
                               uint8_t* packet, uint16_t size);
static void a2dp_source_handler(uint8_t pt, uint16_t ch,
                                uint8_t* packet, uint16_t size);
static void attempt_connect_fn(void);
static void reset_link_state(void);
static void wasapi_invalidated_cb(void);
static void log_line(const char* fmt, ...);
static void snapshot_set_state(engine_state_t s);

// ── Public API ─────────────────────────────────────────────────────────
int engine_init(const engine_config_t* cfg) {
    if (!cfg) return -1;
    memset(&e, 0, sizeof(e));
    memcpy(e.target, cfg->target_addr, sizeof(bd_addr_t));
    if (cfg->local_name) {
        btstack_strcpy(e.local_name, sizeof(e.local_name), cfg->local_name);
    } else {
        btstack_strcpy(e.local_name, sizeof(e.local_name), "win-ldac");
    }
    e.reconnect_interval_ms = cfg->reconnect_interval_ms ?
                              cfg->reconnect_interval_ms : 5000;
    e.bitrate_mode      = cfg->initial_bitrate_mode;
    e.a2dp_bitrate_mode = (e.bitrate_mode == ENGINE_BITRATE_ADAPTIVE)
                          ? A2DP_LDAC_BITRATE_ADAPTIVE
                          : A2DP_LDAC_BITRATE_FIXED_HQ;
    e.state             = ENGINE_STATE_IDLE;
    InitializeCriticalSection(&e.snapshot_lock);
    e.snapshot_lock_inited = true;

    // 1. WASAPI — get the mixer rate, used to drive LDAC negotiation.
    int rc = wasapi_loopback_init(&e.wasapi_sample_rate, &e.wasapi_channels);
    if (rc != 0) {
        log_line("[engine] wasapi_loopback_init rc=%d", rc);
        return -1;
    }
    log_line("[engine] WASAPI: %d Hz, %d ch, device bit-depth %d",
             e.wasapi_sample_rate, e.wasapi_channels,
             wasapi_loopback_device_bit_depth());
    if (e.wasapi_channels != 2 ||
        !a2dp_ldac_hz_to_sf_bit(e.wasapi_sample_rate)) {
        log_line("[engine] mixer must be stereo @ 44.1/48/88.2/96 kHz");
        return -1;
    }
    wasapi_loopback_set_invalidated_callback(&wasapi_invalidated_cb);

    if (wasapi_loopback_start() != 0) {
        log_line("[engine] wasapi_loopback_start failed");
        return -1;
    }

    // 2. BTstack core.
    btstack_memory_init();
    btstack_run_loop_init(btstack_run_loop_windows_get_instance());
    hci_init(hci_transport_usb_instance(), NULL);

    // Persistent link key store so the XM5 doesn't ask to re-pair every
    // session (M6.1).
    hci_set_link_key_db(win_ldac_link_key_db_instance());
    log_line("[engine] link key store: %s", win_ldac_link_key_db_path());

    e.hci_cb_reg.callback = &hci_packet_handler;
    hci_add_event_handler(&e.hci_cb_reg);

    char fw_dir[MAX_PATH];
    if (resolve_firmware_folder(fw_dir, sizeof(fw_dir))) {
        log_line("[engine] Realtek firmware folder: %s", fw_dir);
        btstack_chipset_realtek_set_firmware_folder_path(fw_dir);
        btstack_chipset_realtek_set_config_folder_path(fw_dir);
        btstack_chipset_realtek_set_product_id(RTL8761BU_CANONICAL_PID);
        hci_set_chipset(btstack_chipset_realtek_instance());
        hci_enable_custom_pre_init();
    } else {
        log_line("[engine] WARNING: firmware folder not found near exe");
    }

    hci_set_master_slave_policy(0);
    hci_set_inquiry_mode(INQUIRY_MODE_RSSI_AND_EIR);
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    gap_ssp_set_auto_accept(1);

    l2cap_init();
    a2dp_source_init();
    a2dp_source_register_packet_handler(&a2dp_source_handler);

    // Advertise full LDAC sample-rate support; stereo only.
    a2dp_ldac_build_capability(e.ldac_caps,
                               A2DP_LDAC_SF_ALL, A2DP_LDAC_CM_STEREO);
    avdtp_stream_endpoint_t* ldac_se = a2dp_source_create_stream_endpoint(
        AVDTP_AUDIO, AVDTP_CODEC_NON_A2DP,
        e.ldac_caps, sizeof(e.ldac_caps),
        e.ldac_config_buf, sizeof(e.ldac_config_buf));
    if (!ldac_se) {
        log_line("[engine] could not create LDAC SEP");
        return -1;
    }
    e.local_seid = avdtp_local_seid(ldac_se);
    log_line("[engine] LDAC SEP local_seid=%u", e.local_seid);

    sdp_init();
    memset(e.sdp_a2dp_source_buf, 0, sizeof(e.sdp_a2dp_source_buf));
    a2dp_source_create_sdp_record(e.sdp_a2dp_source_buf,
                                   sdp_create_service_record_handle(),
                                   AVDTP_SOURCE_FEATURE_MASK_PLAYER,
                                   NULL, NULL);
    sdp_register_service(e.sdp_a2dp_source_buf);

    memset(e.sdp_device_id_buf, 0, sizeof(e.sdp_device_id_buf));
    device_id_create_sdp_record(e.sdp_device_id_buf,
                                 sdp_create_service_record_handle(),
                                 DEVICE_ID_VENDOR_ID_SOURCE_BLUETOOTH,
                                 BLUETOOTH_COMPANY_ID_BLUEKITCHEN_GMBH,
                                 1, 1);
    sdp_register_service(e.sdp_device_id_buf);

    gap_set_local_name(e.local_name);
    gap_set_class_of_device(CLASS_OF_DEVICE);
    gap_discoverable_control(1);

    reconnect_supervisor_init(e.target, &attempt_connect_fn,
                              e.reconnect_interval_ms);

    snapshot_set_state(ENGINE_STATE_HCI_INIT);
    return 0;
}

void engine_run(void) {
    hci_power_control(HCI_POWER_ON);
    btstack_run_loop_execute();
}

static btstack_context_callback_registration_t stop_cb_storage;
static void on_bt_thread_stop(void* arg) {
    (void)arg;
    log_line("[engine] stop requested → powering HCI off");
    hci_power_control(HCI_POWER_OFF);
}
void engine_request_stop(void) {
    if (InterlockedExchange(&e.stop_requested, 1) != 0) return;  // idempotent
    stop_cb_storage.callback = &on_bt_thread_stop;
    stop_cb_storage.context  = NULL;
    btstack_run_loop_execute_on_main_thread(&stop_cb_storage);
}

void engine_shutdown(void) {
    a2dp_ldac_source_teardown();
    wasapi_loopback_stop();
    if (e.snapshot_lock_inited) {
        DeleteCriticalSection(&e.snapshot_lock);
        e.snapshot_lock_inited = false;
    }
}

void engine_get_status_snapshot(engine_status_t* out) {
    if (!out) return;
    EnterCriticalSection(&e.snapshot_lock);
    memcpy(out->target_addr, e.target, sizeof(bd_addr_t));
    out->state              = e.state;
    out->link_up            = e.link_up;
    out->sample_rate_hz     = a2dp_ldac_source_negotiated_sample_rate();
    out->wasapi_bit_depth   = wasapi_loopback_device_bit_depth();
    out->effective_kbps     = a2dp_ldac_source_effective_kbps();
    out->nominal_kbps       = a2dp_ldac_source_nominal_kbps();
    out->eqmid              = a2dp_ldac_source_current_eqmid();
    out->bitrate_mode       = e.bitrate_mode;
    out->underrun_samples   = a2dp_ldac_source_underrun_samples();
    out->reconnect_attempts = reconnect_supervisor_attempt_count();
    LeaveCriticalSection(&e.snapshot_lock);
}

// ── Cross-thread commands ──────────────────────────────────────────────
// These piggyback on btstack_run_loop_execute_on_main_thread, which lets
// arbitrary code be scheduled onto the BT thread regardless of where the
// caller is. The runtime registration struct holds a function pointer +
// context.

typedef struct {
    btstack_context_callback_registration_t cb;
    engine_bitrate_mode_t mode;
} set_bitrate_mode_msg_t;
static set_bitrate_mode_msg_t set_bitrate_msg_storage;

static void on_bt_thread_set_bitrate_mode(void* arg) {
    set_bitrate_mode_msg_t* m = (set_bitrate_mode_msg_t*)arg;
    EnterCriticalSection(&e.snapshot_lock);
    e.bitrate_mode = m->mode;
    e.a2dp_bitrate_mode = (m->mode == ENGINE_BITRATE_ADAPTIVE)
                          ? A2DP_LDAC_BITRATE_ADAPTIVE
                          : A2DP_LDAC_BITRATE_FIXED_HQ;
    LeaveCriticalSection(&e.snapshot_lock);
    a2dp_ldac_source_set_bitrate_mode(e.a2dp_bitrate_mode);
    log_line("[engine] bitrate mode → %s",
             m->mode == ENGINE_BITRATE_ADAPTIVE ? "Adaptive" : "Fixed HQ");
}

void engine_post_set_bitrate_mode(engine_bitrate_mode_t mode) {
    set_bitrate_msg_storage.mode = mode;
    set_bitrate_msg_storage.cb.callback = &on_bt_thread_set_bitrate_mode;
    set_bitrate_msg_storage.cb.context  = &set_bitrate_msg_storage;
    btstack_run_loop_execute_on_main_thread(&set_bitrate_msg_storage.cb);
}

typedef struct {
    btstack_context_callback_registration_t cb;
    bd_addr_t addr;
} set_target_msg_t;
static set_target_msg_t set_target_msg_storage;

static void on_bt_thread_set_target(void* arg) {
    set_target_msg_t* m = (set_target_msg_t*)arg;
    memcpy(e.target, m->addr, sizeof(bd_addr_t));
    log_line("[engine] target → %s", bd_addr_to_str(e.target));
    // Drop the current link (if any) and let the supervisor reconnect
    // to the new address.
    if (e.a2dp_cid) {
        a2dp_source_disconnect(e.a2dp_cid);
    } else {
        reconnect_supervisor_note_disconnected();
    }
}

void engine_post_set_target(const bd_addr_t addr) {
    memcpy(set_target_msg_storage.addr, addr, sizeof(bd_addr_t));
    set_target_msg_storage.cb.callback = &on_bt_thread_set_target;
    set_target_msg_storage.cb.context  = &set_target_msg_storage;
    btstack_run_loop_execute_on_main_thread(&set_target_msg_storage.cb);
}

void engine_set_log_callback(engine_log_fn fn) {
    e.log_fn = fn;
}

// ── Internals ──────────────────────────────────────────────────────────
static void log_line(const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (e.log_fn) {
        e.log_fn(line);
    } else {
        puts(line);
    }
}

static void snapshot_set_state(engine_state_t s) {
    EnterCriticalSection(&e.snapshot_lock);
    e.state = s;
    LeaveCriticalSection(&e.snapshot_lock);
}

static void snapshot_set_link_up(bool up) {
    EnterCriticalSection(&e.snapshot_lock);
    e.link_up = up;
    LeaveCriticalSection(&e.snapshot_lock);
}

// Reset per-link state. Negotiation cache (negotiated_sr_hz / cm_bit /
// set_config_payload[]) is intentionally preserved across reconnects —
// see comment in a2dp_ldac_persistent_test (M6.2) and the M7 cid 0x0002
// fix.
static void reset_link_state(void) {
    if (e.streaming_started) {
        a2dp_ldac_source_stop();
    }
    a2dp_ldac_source_teardown();

    e.a2dp_cid             = 0;
    e.remote_seid          = 0;
    e.stream_opened        = false;
    e.ldac_capability_seen = false;
    e.config_applied       = false;
    e.streaming_started    = false;
    snapshot_set_link_up(false);
}

static void attempt_connect_fn(void) {
    reset_link_state();
    snapshot_set_state(ENGINE_STATE_CONNECTING);
    uint8_t rc = a2dp_source_establish_stream(e.target, &e.a2dp_cid);
    if (rc != ERROR_CODE_SUCCESS) {
        log_line("[engine] establish_stream rc=0x%02x — will retry", rc);
        reconnect_supervisor_note_disconnected();
    }
}

// Triggered on the WASAPI capture thread when Windows reports the audio
// endpoint format has changed (user altered Sound Properties, default
// device switched, etc.). We can't safely call BTstack from here, so we
// post a callback to the BT thread; the actual rebuild happens there
// where it's free to join the now-exiting capture thread.
static btstack_context_callback_registration_t wasapi_invalidated_cb_reg;
static volatile LONG wasapi_invalidated_pending;

static void on_bt_thread_rebuild_wasapi(void* arg) {
    (void)arg;
    InterlockedExchange(&wasapi_invalidated_pending, 0);
    log_line("[engine] rebuilding WASAPI after device-format change");

    // The capture thread has already exited (the invalidated branch in
    // wasapi_loopback's loop falls into thread_exit). Stop joins it and
    // tears down the COM interfaces.
    wasapi_loopback_stop();

    int new_sr = 0, new_ch = 0;
    int rc = wasapi_loopback_init(&new_sr, &new_ch);
    if (rc != 0) {
        log_line("[engine] WASAPI re-init failed rc=%d — giving up", rc);
        return;
    }
    e.wasapi_sample_rate = new_sr;
    e.wasapi_channels    = new_ch;
    log_line("[engine] WASAPI rebuilt: %d Hz / %d-bit",
             new_sr, wasapi_loopback_device_bit_depth());
    wasapi_loopback_set_invalidated_callback(&wasapi_invalidated_cb);
    if (wasapi_loopback_start() != 0) {
        log_line("[engine] wasapi_loopback_start failed");
        return;
    }

    // Cached LDAC negotiation parameters refer to the OLD WASAPI rate.
    // Clear them so the next OTHER_CAPABILITY exchange re-derives the
    // sample rate from the fresh WASAPI value.
    e.negotiated_sr_hz  = 0;
    e.negotiated_cm_bit = 0;

    // Force a reconnect so SET_CONFIGURATION goes out with the new
    // sample rate. The supervisor handles the rest.
    if (e.a2dp_cid) {
        a2dp_source_disconnect(e.a2dp_cid);
    } else {
        reconnect_supervisor_note_disconnected();
    }
}

static void wasapi_invalidated_cb(void) {
    // Coalesce repeat fires: WASAPI can deliver multiple invalidations
    // back-to-back if the device transitions through intermediate
    // states. We only need one rebuild.
    if (InterlockedExchange(&wasapi_invalidated_pending, 1) != 0) return;
    wasapi_invalidated_cb_reg.callback = &on_bt_thread_rebuild_wasapi;
    wasapi_invalidated_cb_reg.context  = NULL;
    btstack_run_loop_execute_on_main_thread(&wasapi_invalidated_cb_reg);
}

// ── Event handlers ─────────────────────────────────────────────────────
static void hci_packet_handler(uint8_t pt, uint16_t ch,
                               uint8_t* packet, uint16_t size) {
    (void)ch; (void)size;
    if (pt != HCI_EVENT_PACKET) return;
    switch (hci_event_packet_get_type(packet)) {
    case BTSTACK_EVENT_STATE: {
        uint8_t st = btstack_event_state_get_state(packet);
        if (st == HCI_STATE_WORKING) {
            bd_addr_t local;
            gap_local_bd_addr(local);
            log_line("[engine] HCI up. Local %s. Starting supervisor for %s",
                     bd_addr_to_str(local), bd_addr_to_str(e.target));
            snapshot_set_state(ENGINE_STATE_DISCONNECTED);
            reconnect_supervisor_start();
        } else if (st == HCI_STATE_OFF) {
            log_line("[engine] HCI off");
            snapshot_set_state(ENGINE_STATE_STOPPING);
            exit(0);
        }
        break;
    }
    case BTSTACK_EVENT_POWERON_FAILED:
        log_line("[engine] HCI power on failed");
        exit(3);

    case HCI_EVENT_DISCONNECTION_COMPLETE:
        log_line("[engine] HCI baseband disconnected");
        reset_link_state();
        reconnect_supervisor_note_disconnected();
        snapshot_set_state(ENGINE_STATE_DISCONNECTED);
        break;

    case HCI_EVENT_USER_CONFIRMATION_REQUEST: {
        bd_addr_t addr;
        hci_event_user_confirmation_request_get_bd_addr(packet, addr);
        log_line("[engine] SSP user confirmation from %s — auto-accept",
                 bd_addr_to_str(addr));
        break;
    }
    case HCI_EVENT_PIN_CODE_REQUEST: {
        bd_addr_t addr;
        hci_event_pin_code_request_get_bd_addr(packet, addr);
        gap_pin_code_response(addr, "0000");
        break;
    }
    default: break;
    }
}

static void a2dp_source_handler(uint8_t pt, uint16_t ch,
                                uint8_t* packet, uint16_t size) {
    (void)ch; (void)size;
    if (pt != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != HCI_EVENT_A2DP_META) return;

    uint8_t sub = hci_event_a2dp_meta_get_subevent_code(packet);
    switch (sub) {

    case A2DP_SUBEVENT_SIGNALING_CONNECTION_ESTABLISHED: {
        uint8_t status =
            a2dp_subevent_signaling_connection_established_get_status(packet);
        if (status != ERROR_CODE_SUCCESS) {
            log_line("[engine] signalling connect failed (status=0x%02x)",
                     status);
            reconnect_supervisor_note_disconnected();
            snapshot_set_state(ENGINE_STATE_DISCONNECTED);
            break;
        }
        e.a2dp_cid =
            a2dp_subevent_signaling_connection_established_get_a2dp_cid(packet);
        reconnect_supervisor_note_connected();
        snapshot_set_link_up(true);
        snapshot_set_state(ENGINE_STATE_NEGOTIATING);
        log_line("[engine] signalling connected (cid 0x%04x)", e.a2dp_cid);
        break;
    }

    case A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_OTHER_CAPABILITY: {
        uint8_t remote_seid =
            a2dp_subevent_signaling_media_codec_other_capability_get_remote_seid(packet);
        uint16_t info_len =
            a2dp_subevent_signaling_media_codec_other_capability_get_media_codec_information_len(packet);
        const uint8_t* info =
            a2dp_subevent_signaling_media_codec_other_capability_get_media_codec_information(packet);

        if (!a2dp_ldac_is_ldac_payload(info, info_len)) break;

        uint8_t remote_sr_bitmap = info[6];
        uint8_t remote_cm_bitmap = info[7];
        uint8_t our_sr_bitmap    = e.ldac_caps[6];
        uint8_t our_cm_bitmap    = e.ldac_caps[7];
        uint8_t common_sr = remote_sr_bitmap & our_sr_bitmap;
        uint8_t common_cm = remote_cm_bitmap & our_cm_bitmap;

        uint8_t chosen_sr =
            a2dp_ldac_pick_sample_rate_for_hz(common_sr, e.wasapi_sample_rate);
        uint8_t chosen_cm = a2dp_ldac_pick_best_channel_mode(common_cm);
        if (chosen_sr == 0 || chosen_cm == 0) {
            log_line("[engine] no common LDAC sr/cm — disconnect & retry");
            a2dp_source_disconnect(e.a2dp_cid);
            break;
        }

        a2dp_ldac_build_config(e.set_config_payload, chosen_sr, chosen_cm);
        e.negotiated_sr_hz  = a2dp_ldac_sf_bit_to_hz(chosen_sr);
        e.negotiated_cm_bit = chosen_cm;

        if (e.config_applied) break;
        e.config_applied = true;
        e.ldac_capability_seen = true;

        uint8_t rc = a2dp_source_set_config_other(
            e.a2dp_cid, e.local_seid, remote_seid,
            e.set_config_payload, sizeof(e.set_config_payload));
        if (rc != ERROR_CODE_SUCCESS) {
            log_line("[engine] set_config_other rc=0x%02x — disconnect", rc);
            a2dp_source_disconnect(e.a2dp_cid);
            break;
        }
        e.remote_seid = remote_seid;
        log_line("[engine] LDAC %s STEREO negotiated; awaiting STREAM_ESTABLISHED",
                 a2dp_ldac_sf_label(chosen_sr));
        break;
    }

    case A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_OTHER_CONFIGURATION:
        break;

    case A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_SBC_CONFIGURATION:
        log_line("[engine] XM5 picked SBC — disconnect");
        a2dp_source_disconnect(e.a2dp_cid);
        break;

    case A2DP_SUBEVENT_STREAM_ESTABLISHED: {
        uint8_t status = a2dp_subevent_stream_established_get_status(packet);
        if (status != ERROR_CODE_SUCCESS) {
            log_line("[engine] STREAM_ESTABLISHED failed (status=0x%02x)",
                     status);
            a2dp_source_disconnect(e.a2dp_cid);
            break;
        }
        e.stream_opened = true;

        if (e.negotiated_sr_hz == 0 || e.negotiated_cm_bit == 0) {
            log_line("[engine] STREAM_ESTABLISHED with no LDAC caps — "
                     "disconnect & retry");
            a2dp_source_disconnect(e.a2dp_cid);
            break;
        }

        int rc = a2dp_ldac_source_setup(
            e.a2dp_cid, e.local_seid,
            e.negotiated_sr_hz,
            e.negotiated_cm_bit,
            e.a2dp_bitrate_mode,
            wasapi_loopback_read_f32);
        if (rc != 0) {
            log_line("[engine] a2dp_ldac_source_setup failed");
            a2dp_source_disconnect(e.a2dp_cid);
            break;
        }
        log_line("[engine] encoder up @ %d Hz, mode=%s",
                 e.negotiated_sr_hz,
                 e.a2dp_bitrate_mode == A2DP_LDAC_BITRATE_ADAPTIVE
                     ? "Adaptive" : "Fixed HQ");
        a2dp_source_start_stream(e.a2dp_cid, e.local_seid);
        break;
    }

    case A2DP_SUBEVENT_STREAM_STARTED:
        if (!e.streaming_started) {
            // First STREAM_STARTED after STREAM_ESTABLISHED — start the
            // audio timer.
            a2dp_ldac_source_start();
            e.streaming_started = true;
            log_line("[engine] streaming");
        } else {
            // Resume after a silence-driven AVDTP SUSPEND. Audio timer
            // is already running.
            a2dp_ldac_source_on_avdtp_started();
            log_line("[engine] streaming (resumed)");
        }
        snapshot_set_state(ENGINE_STATE_STREAMING);
        break;

    case A2DP_SUBEVENT_STREAMING_CAN_SEND_MEDIA_PACKET_NOW:
        a2dp_ldac_source_on_can_send_now();
        break;

    case A2DP_SUBEVENT_STREAM_SUSPENDED:
        // AVDTP SUSPEND — either we asked for it (silence detector) or
        // the remote did. Keep the audio timer running so the silence
        // watcher can resume the stream when audio reappears.
        log_line("[engine] stream suspended (idle)");
        a2dp_ldac_source_on_avdtp_suspended();
        // Stay in STREAMING state from the engine's POV; the link is
        // up, we're just temporarily not pushing data.
        break;

    case A2DP_SUBEVENT_STREAM_RELEASED:
        log_line("[engine] stream released (underrun=%llu)",
                 (unsigned long long)a2dp_ldac_source_underrun_samples());
        a2dp_ldac_source_stop();
        e.streaming_started = false;
        e.stream_opened = false;
        snapshot_set_state(ENGINE_STATE_NEGOTIATING);
        break;

    case A2DP_SUBEVENT_SIGNALING_CONNECTION_RELEASED:
        log_line("[engine] signalling released");
        reset_link_state();
        reconnect_supervisor_note_disconnected();
        snapshot_set_state(ENGINE_STATE_DISCONNECTED);
        break;

    default:
        break;
    }
}

