// a2dp_ldac_persistent_test — M6.2 acceptance test.
//
// Like a2dp_ldac_play_test (M5), but stays up and reconnects:
//
//   - First start: pages the XM5, negotiates LDAC, streams.
//   - XM5 powers off / leaves range:
//       AVDTP signalling drops → encoder is torn down → reconnect
//       supervisor schedules another connect attempt every 5 s.
//   - XM5 powers back on / returns to range:
//       Next attempt succeeds → renegotiate (same link key from M6.1's
//       persistent DB, so no re-pair prompt) → rebuild encoder → music
//       resumes flowing.
//
// WASAPI loopback stays running the entire time. While the link is
// down, ring-buffer overflow just drops old PCM — we don't bother
// stopping it, because the moment we reconnect we want the live audio,
// not stale buffered audio.
//
// Acceptance:
//   1. Start the program, music plays through XM5 (smoke test, M5 path).
//   2. Power off XM5 → music stops → console shows reconnect attempts.
//   3. Power on XM5 → within ~10 s music resumes (no re-pair, no user
//      action).
//   4. Repeat. Loop indefinitely.
//   5. Press Ctrl+C to terminate; the program exits non-cleanly (this
//      is fine for v1; XM5 sees signalling timeout on its side).

#include <inttypes.h>
#include <stdint.h>
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

// ── Configuration ──────────────────────────────────────────────────────
#define TARGET_DEVICE_ADDR        "88:C9:E8:F7:D5:F3"
#define LOCAL_NAME                "win-ldac (M6.2)"
#define CLASS_OF_DEVICE           0x200408
#define RTL8761BU_CANONICAL_PID   0x8771
#define RECONNECT_INTERVAL_MS     5000

// ── State ──────────────────────────────────────────────────────────────
// Per-link state. Everything that's per-connection (cids, the negotiated
// config, flags tracking progress through the AVDTP handshake) lives
// here and gets cleared every time we attempt a new connect.
typedef struct {
    uint16_t a2dp_cid;
    uint8_t  local_seid;
    uint8_t  remote_seid;
    bool     stream_opened;
    bool     ldac_capability_seen;
    bool     config_applied;
    bool     streaming_started;

    // Static-lifetime SET_CONFIGURATION buffer (BTstack stashes the
    // pointer, not the bytes). Overwritten on each negotiation.
    uint8_t  set_config_payload[A2DP_LDAC_CAPS_LEN];
    int      negotiated_sr_hz;
    uint8_t  negotiated_cm_bit;
} link_state_t;
static link_state_t link;

static uint8_t ldac_caps[A2DP_LDAC_CAPS_LEN];
static uint8_t ldac_config_buf[A2DP_LDAC_CAPS_LEN];
static uint8_t sdp_a2dp_source_buf[150];
static uint8_t sdp_device_id_buf[100];

static int wasapi_sample_rate = 0;
static int wasapi_channels    = 0;
static bd_addr_t target_addr;

static btstack_packet_callback_registration_t hci_event_callback_registration;

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

// Reset per-link state so the next negotiation starts clean. Used both
// at startup and on every disconnect/reconnect cycle. Does NOT touch
// WASAPI — that stays running for the program's lifetime.
//
// Important: we deliberately do NOT clear negotiated_sr_hz /
// negotiated_cm_bit / set_config_payload. BTstack sometimes skips the
// OTHER_CAPABILITY emit on quick reconnects (M6.2 surfaced this as the
// "cid 0x0002 channel mode 0x00" failure), and the XM5's LDAC caps are
// stable per device anyway. Keeping the cached values means a "no
// capability re-advertisement" reconnect can still go through using
// what we learned on the first successful negotiation.
static void reset_link_state(void) {
    if (link.streaming_started) {
        a2dp_ldac_source_stop();
    }
    a2dp_ldac_source_teardown();

    link.a2dp_cid             = 0;
    link.remote_seid          = 0;
    link.stream_opened        = false;
    link.ldac_capability_seen = false;
    link.config_applied       = false;
    link.streaming_started    = false;
    // Intentionally preserved: negotiated_sr_hz, negotiated_cm_bit,
    // set_config_payload[].
}

// Attempt callback invoked by the supervisor. Called from the run-loop
// thread.
static void attempt_connect(void) {
    reset_link_state();
    uint8_t rc = a2dp_source_establish_stream(target_addr, &link.a2dp_cid);
    if (rc != ERROR_CODE_SUCCESS) {
        // Synchronous failure (BTstack didn't accept the request, e.g.
        // because the HCI layer is busy or another connection is being
        // torn down). Treat as a failed attempt; supervisor reschedules.
        fprintf(stderr,
            "[!!] establish_stream returned 0x%02x — will retry\n", rc);
        reconnect_supervisor_note_disconnected();
    }
    // SUCCESS just means the request was queued; the actual result
    // arrives later as A2DP_SUBEVENT_SIGNALING_CONNECTION_ESTABLISHED.
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
            printf("[OK] HCI up. Local %s. Starting supervisor for %s ...\n",
                   bd_addr_to_str(local), TARGET_DEVICE_ADDR);
            // Kick off the first connect attempt. Subsequent ones are
            // driven by note_disconnected() in the A2DP handler.
            reconnect_supervisor_start();
        } else if (st == HCI_STATE_OFF) {
            printf("HCI off. Bye.\n");
            exit(0);
        }
        break;
    }
    case BTSTACK_EVENT_POWERON_FAILED:
        fprintf(stderr,
                "[ERR] Power on failed. Check Zadig + Memory Integrity.\n");
        exit(3);

    case HCI_EVENT_DISCONNECTION_COMPLETE:
        // Defensive: even if A2DP signalling didn't surface a release
        // (e.g. sudden RF loss), the baseband layer will. Funnel into
        // the supervisor so a retry gets scheduled. note_disconnected
        // is idempotent so it's safe to double-fire alongside the A2DP
        // event below.
        printf("[..] HCI baseband disconnected\n");
        reset_link_state();
        reconnect_supervisor_note_disconnected();
        break;

    case HCI_EVENT_USER_CONFIRMATION_REQUEST: {
        bd_addr_t addr;
        hci_event_user_confirmation_request_get_bd_addr(packet, addr);
        printf("[..] SSP user confirmation from %s — auto-accept\n",
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
            // Page timeout, refused, paired-but-busy, etc. Schedule a
            // retry; reset_link_state was already done in attempt_connect.
            fprintf(stderr,
                "[!!] A2DP signalling connect failed (status=0x%02x). "
                "Retrying...\n", status);
            reconnect_supervisor_note_disconnected();
            break;
        }
        link.a2dp_cid =
            a2dp_subevent_signaling_connection_established_get_a2dp_cid(packet);
        reconnect_supervisor_note_connected();
        printf("[OK] A2DP signalling connected (cid 0x%04x). "
               "Waiting for XM5 LDAC capability ...\n",
               link.a2dp_cid);
        break;
    }

    case A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_OTHER_CAPABILITY: {
        uint8_t remote_seid =
            a2dp_subevent_signaling_media_codec_other_capability_get_remote_seid(packet);
        uint16_t info_len =
            a2dp_subevent_signaling_media_codec_other_capability_get_media_codec_information_len(packet);
        const uint8_t* info =
            a2dp_subevent_signaling_media_codec_other_capability_get_media_codec_information(packet);

        if (!a2dp_ldac_is_ldac_payload(info, info_len)) {
            printf("[..] non-LDAC vendor codec capability "
                   "(remote_seid=%u), ignoring\n", remote_seid);
            break;
        }

        uint8_t remote_sr_bitmap = info[6];
        uint8_t remote_cm_bitmap = info[7];
        uint8_t our_sr_bitmap    = ldac_caps[6];
        uint8_t our_cm_bitmap    = ldac_caps[7];
        uint8_t common_sr = remote_sr_bitmap & our_sr_bitmap;
        uint8_t common_cm = remote_cm_bitmap & our_cm_bitmap;

        uint8_t chosen_sr =
            a2dp_ldac_pick_sample_rate_for_hz(common_sr, wasapi_sample_rate);
        uint8_t chosen_cm = a2dp_ldac_pick_best_channel_mode(common_cm);
        if (chosen_sr == 0 || chosen_cm == 0) {
            fprintf(stderr,
                "[!!] No common sample rate or channel mode "
                "(sr=0x%02X cm=0x%02X) — disconnecting & retrying\n",
                common_sr, common_cm);
            a2dp_source_disconnect(link.a2dp_cid);
            break;
        }

        a2dp_ldac_build_config(link.set_config_payload, chosen_sr, chosen_cm);
        link.negotiated_sr_hz  = a2dp_ldac_sf_bit_to_hz(chosen_sr);
        link.negotiated_cm_bit = chosen_cm;

        if (link.config_applied) break;
        link.config_applied       = true;
        link.ldac_capability_seen = true;

        uint8_t rc = a2dp_source_set_config_other(
            link.a2dp_cid, link.local_seid, remote_seid,
            link.set_config_payload, sizeof(link.set_config_payload));
        if (rc != ERROR_CODE_SUCCESS) {
            fprintf(stderr,
                "[!!] set_config_other rc=0x%02x — disconnecting & "
                "retrying\n", rc);
            a2dp_source_disconnect(link.a2dp_cid);
            break;
        }
        link.remote_seid = remote_seid;
        printf("[OK] negotiated LDAC %s STEREO. SET_CONFIGURATION queued.\n",
               a2dp_ldac_sf_label(chosen_sr));
        break;
    }

    case A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_OTHER_CONFIGURATION:
        // BTstack drives the rest of AVDTP from here; just wait for
        // STREAM_ESTABLISHED. Nothing to do.
        break;

    case A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_SBC_CONFIGURATION:
        fprintf(stderr,
            "[!!] XM5 picked SBC (we offered only LDAC) — disconnecting\n");
        a2dp_source_disconnect(link.a2dp_cid);
        break;

    case A2DP_SUBEVENT_STREAM_ESTABLISHED: {
        uint8_t status = a2dp_subevent_stream_established_get_status(packet);
        if (status != ERROR_CODE_SUCCESS) {
            fprintf(stderr,
                "[!!] STREAM_ESTABLISHED failed (status=0x%02x) — "
                "disconnecting & retrying\n", status);
            a2dp_source_disconnect(link.a2dp_cid);
            break;
        }
        link.stream_opened = true;

        if (link.negotiated_sr_hz == 0 || link.negotiated_cm_bit == 0) {
            // OTHER_CAPABILITY was never delivered on this connection
            // and we have no cached value from a prior session. Can't
            // configure the encoder without it — disconnect so the
            // supervisor retries; usually the next attempt does get the
            // capability event.
            fprintf(stderr,
                "[!!] STREAM_ESTABLISHED before LDAC capability seen — "
                "disconnecting & retrying\n");
            a2dp_source_disconnect(link.a2dp_cid);
            break;
        }
        int rc = a2dp_ldac_source_setup(
            link.a2dp_cid, link.local_seid,
            link.negotiated_sr_hz,
            link.negotiated_cm_bit,
            A2DP_LDAC_BITRATE_FIXED_HQ,
            wasapi_loopback_read_f32);
        if (rc != 0) {
            fprintf(stderr,
                "[!!] a2dp_ldac_source_setup failed — disconnecting\n");
            a2dp_source_disconnect(link.a2dp_cid);
            break;
        }
        printf("[OK] LDAC encoder up @ %d Hz, EQMID=HQ. Starting stream...\n",
               link.negotiated_sr_hz);
        a2dp_source_start_stream(link.a2dp_cid, link.local_seid);
        break;
    }

    case A2DP_SUBEVENT_STREAM_STARTED:
        if (!link.streaming_started) {
            a2dp_ldac_source_start();
            link.streaming_started = true;
            printf("[OK] Streaming. Play audio on Windows; XM5 will hear it.\n"
                   "    Power XM5 off / on to test reconnect.\n");
        } else {
            a2dp_ldac_source_on_avdtp_started();
            printf("[..] Streaming resumed (audio returned)\n");
        }
        break;

    case A2DP_SUBEVENT_STREAMING_CAN_SEND_MEDIA_PACKET_NOW:
        a2dp_ldac_source_on_can_send_now();
        break;

    case A2DP_SUBEVENT_STREAM_SUSPENDED:
        // AVDTP-suspended due to silence; the audio timer keeps running
        // so the silence watcher can resume the stream when audio
        // returns. Don't clear streaming_started — STREAM_STARTED on
        // resume will use it to take the "resumed" path.
        printf("[..] Stream suspended (idle)\n");
        a2dp_ldac_source_on_avdtp_suspended();
        break;

    case A2DP_SUBEVENT_STREAM_RELEASED:
        printf("[..] Stream released (underrun samples = %" PRIu64 ")\n",
               a2dp_ldac_source_underrun_samples());
        // STREAM_RELEASED can fire from the sink's side without the
        // signalling link going down — but in practice for the XM5
        // power-off case the signalling release follows immediately.
        // We leave the supervisor untouched here; the signalling
        // handler below takes care of reconnect scheduling.
        a2dp_ldac_source_stop();
        link.streaming_started = false;
        link.stream_opened = false;
        break;

    case A2DP_SUBEVENT_SIGNALING_CONNECTION_RELEASED:
        printf("[..] Signalling connection released\n");
        reset_link_state();
        reconnect_supervisor_note_disconnected();
        break;

    default:
        break;
    }
}

// ── Main ───────────────────────────────────────────────────────────────
int main(int argc, char** argv) {
    (void)argc; (void)argv;
    setvbuf(stdout, NULL, _IONBF, 0);
    SetConsoleOutputCP(CP_UTF8);

    printf("a2dp_ldac_persistent_test - M6.2 acceptance (auto-reconnect)\n");
    printf("============================================================\n");
    printf("Target: %s.  Retry interval: %d ms.\n",
           TARGET_DEVICE_ADDR, RECONNECT_INTERVAL_MS);
    printf("Press Ctrl+C to quit (XM5 will see a signalling timeout).\n\n");

    // 1. WASAPI first, so we know the mixer rate before negotiating LDAC.
    int rc = wasapi_loopback_init(&wasapi_sample_rate, &wasapi_channels);
    if (rc != 0) {
        fprintf(stderr, "[ERR] wasapi_loopback_init rc=%d\n", rc);
        return 2;
    }
    printf("[OK] WASAPI loopback: %d Hz, %d ch\n",
           wasapi_sample_rate, wasapi_channels);
    if (wasapi_channels != 2 ||
        !a2dp_ldac_hz_to_sf_bit(wasapi_sample_rate)) {
        fprintf(stderr,
            "[ERR] Windows mixer must be stereo @ 44.1/48/88.2/96 kHz.\n");
        return 2;
    }
    if (wasapi_loopback_start() != 0) {
        fprintf(stderr, "[ERR] wasapi_loopback_start failed\n");
        return 2;
    }
    // WASAPI now runs for the program's lifetime. While the link is
    // down, the ring buffer just churns through silence (or audio
    // nobody's reading) and drops on overflow.

    sscanf_bd_addr(TARGET_DEVICE_ADDR, target_addr);

    btstack_memory_init();
    btstack_run_loop_init(btstack_run_loop_windows_get_instance());
    hci_init(hci_transport_usb_instance(), NULL);

    // M6.1 link key store: persistent across reboots so the XM5 doesn't
    // demand re-pairing every time we restart.
    hci_set_link_key_db(win_ldac_link_key_db_instance());
    printf("[..] Link key store: %s\n", win_ldac_link_key_db_path());

    hci_event_callback_registration.callback = &hci_packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);

    char fw_dir[MAX_PATH];
    if (resolve_firmware_folder(fw_dir, sizeof(fw_dir))) {
        printf("[..] Realtek firmware folder: %s\n", fw_dir);
        btstack_chipset_realtek_set_firmware_folder_path(fw_dir);
        btstack_chipset_realtek_set_config_folder_path(fw_dir);
        btstack_chipset_realtek_set_product_id(RTL8761BU_CANONICAL_PID);
        hci_set_chipset(btstack_chipset_realtek_instance());
        hci_enable_custom_pre_init();
    }

    hci_set_master_slave_policy(0);
    hci_set_inquiry_mode(INQUIRY_MODE_RSSI_AND_EIR);
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    gap_ssp_set_auto_accept(1);

    l2cap_init();
    a2dp_source_init();
    a2dp_source_register_packet_handler(&a2dp_source_handler);

    a2dp_ldac_build_capability(ldac_caps, A2DP_LDAC_SF_ALL, A2DP_LDAC_CM_STEREO);
    avdtp_stream_endpoint_t* ldac_se = a2dp_source_create_stream_endpoint(
        AVDTP_AUDIO, AVDTP_CODEC_NON_A2DP,
        ldac_caps, sizeof(ldac_caps),
        ldac_config_buf, sizeof(ldac_config_buf));
    if (!ldac_se) {
        fprintf(stderr, "[ERR] could not create LDAC SEP\n");
        return 2;
    }
    link.local_seid = avdtp_local_seid(ldac_se);
    printf("[..] LDAC stream endpoint local_seid=%u\n", link.local_seid);

    sdp_init();
    memset(sdp_a2dp_source_buf, 0, sizeof(sdp_a2dp_source_buf));
    a2dp_source_create_sdp_record(sdp_a2dp_source_buf,
                                   sdp_create_service_record_handle(),
                                   AVDTP_SOURCE_FEATURE_MASK_PLAYER,
                                   NULL, NULL);
    sdp_register_service(sdp_a2dp_source_buf);

    memset(sdp_device_id_buf, 0, sizeof(sdp_device_id_buf));
    device_id_create_sdp_record(sdp_device_id_buf,
                                 sdp_create_service_record_handle(),
                                 DEVICE_ID_VENDOR_ID_SOURCE_BLUETOOTH,
                                 BLUETOOTH_COMPANY_ID_BLUEKITCHEN_GMBH,
                                 1, 1);
    sdp_register_service(sdp_device_id_buf);

    gap_set_local_name(LOCAL_NAME);
    gap_set_class_of_device(CLASS_OF_DEVICE);
    gap_discoverable_control(1);

    // Reconnect supervisor: every RECONNECT_INTERVAL_MS while disconnected,
    // fire attempt_connect. The first attempt happens once HCI is up
    // (see hci_packet_handler / BTSTACK_EVENT_STATE).
    reconnect_supervisor_init(target_addr, &attempt_connect,
                              RECONNECT_INTERVAL_MS);

    hci_power_control(HCI_POWER_ON);
    btstack_run_loop_execute();
    return 0;
}
