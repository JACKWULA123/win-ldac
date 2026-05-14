// a2dp_ldac_play_test — M5 acceptance test.
//
// Combines the M3.2 WASAPI loopback audio source, the M4 LDAC vendor
// codec negotiation, and the new a2dp_ldac_source streaming driver to
// actually push LDAC audio to the XM5. If everything works, this is
// the M5 acceptance milestone: real music from the PC plays through
// the headphones, and Sony Headphones Connect reports the codec as LDAC.
//
// Flow:
//   1. WASAPI loopback bring-up (queries the system mixer rate)
//   2. BTstack + Realtek firmware bring-up
//   3. Connect to XM5 (BD_ADDR captured in M2)
//   4. Register LDAC-only stream endpoint, advertise our capabilities
//   5. On XM5's LDAC capability event, pick a rate that matches WASAPI
//      (prevents resampling) and call set_config_other to drive the
//      vendor SET_CONFIGURATION
//   6. After OTHER_CONFIGURATION accept, wait for STREAM_STARTED
//   7. STREAM_STARTED → start WASAPI capture, configure a2dp_ldac_source,
//      kick off the audio timer
//   8. Run for TEST_DURATION_MS, then suspend + power off.
//
// Acceptance:
//   - XM5 plays whatever Windows is rendering for the test duration
//   - Sony Headphones Connect shows codec = LDAC (not SBC)

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
#include "bt/link_key_db_file.h"

// ── Configuration ──────────────────────────────────────────────────────
#define TARGET_DEVICE_ADDR        "88:C9:E8:F7:D5:F3"
#define LOCAL_NAME                "win-ldac (M5)"
#define CLASS_OF_DEVICE           0x200408
#define RTL8761BU_CANONICAL_PID   0x8771
#define TEST_DURATION_MS          120000   // 2 minutes
#define NEGOTIATION_TIMEOUT_MS    20000

// ── State ──────────────────────────────────────────────────────────────
typedef struct {
    uint16_t a2dp_cid;
    uint8_t  local_seid;
    uint8_t  remote_seid;
    bool     stream_opened;
    bool     ldac_capability_seen;
    bool     config_applied;
    bool     streaming_started;
    bool     test_passed;

    // Static-lifetime buffer for SET_CONFIGURATION. BTstack stores the
    // pointer (not the bytes), then constructs the AVDTP packet later
    // after CAPABILITIES_COMPLETE. See docs/ARCHITECTURE.md §8 / STATUS.md M4.
    uint8_t  set_config_payload[A2DP_LDAC_CAPS_LEN];
    int      negotiated_sr_hz;
    uint8_t  negotiated_cm_bit;
} app_state_t;
static app_state_t app;

static uint8_t ldac_caps[A2DP_LDAC_CAPS_LEN];
static uint8_t ldac_config_buf[A2DP_LDAC_CAPS_LEN];
static uint8_t sdp_a2dp_source_buf[150];
static uint8_t sdp_device_id_buf[100];

static int wasapi_sample_rate = 0;
static int wasapi_channels    = 0;
static bd_addr_t target_addr;

static btstack_packet_callback_registration_t hci_event_callback_registration;
static btstack_timer_source_t total_duration_timer;
static btstack_timer_source_t neg_timeout_timer;

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

static void shutdown_with_status(int rc) {
    app.test_passed = (rc == 0);
    btstack_run_loop_remove_timer(&neg_timeout_timer);
    btstack_run_loop_remove_timer(&total_duration_timer);
    if (app.streaming_started) {
        a2dp_ldac_source_stop();
    }
    a2dp_ldac_source_teardown();
    wasapi_loopback_stop();
    hci_power_control(HCI_POWER_OFF);
}

// ── Event handlers ─────────────────────────────────────────────────────
static void neg_timeout_handler(btstack_timer_source_t* ts) {
    (void)ts;
    if (app.streaming_started) return;  // already past negotiation
    fprintf(stderr,
        "\n[ FAIL ] Negotiation didn't complete in %d s.\n",
        NEGOTIATION_TIMEOUT_MS / 1000);
    shutdown_with_status(2);
}

static void duration_handler(btstack_timer_source_t* ts) {
    (void)ts;
    printf("\n[ PASS ]  M5 test duration elapsed.\n");
    printf("          Underrun samples: %" PRIu64 "\n",
           a2dp_ldac_source_underrun_samples());
    printf("          Nominal LDAC bitrate: %d kbps\n",
           a2dp_ldac_source_nominal_kbps());
    printf("          Effective bitrate:    %d kbps\n",
           a2dp_ldac_source_effective_kbps());
    shutdown_with_status(0);
}

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
            printf("[OK] HCI up. Local %s. Connecting to XM5 %s ...\n",
                   bd_addr_to_str(local), TARGET_DEVICE_ADDR);
            uint8_t rc = a2dp_source_establish_stream(target_addr,
                                                     &app.a2dp_cid);
            if (rc != ERROR_CODE_SUCCESS) {
                fprintf(stderr,
                        "[ERR] establish_stream rc=0x%02x\n", rc);
                shutdown_with_status(4);
            }
        } else if (st == HCI_STATE_OFF) {
            printf("HCI off. %s.\n", app.test_passed ? "Bye" : "Exiting with failure");
            exit(app.test_passed ? 0 : 5);
        }
        break;
    }
    case BTSTACK_EVENT_POWERON_FAILED:
        fprintf(stderr,
                "[ERR] Power on failed. Check Zadig + Memory Integrity.\n");
        exit(3);
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
            fprintf(stderr,
                "[ERR] A2DP signaling connect failed status=0x%02x\n",
                status);
            shutdown_with_status(6);
            break;
        }
        app.a2dp_cid =
            a2dp_subevent_signaling_connection_established_get_a2dp_cid(packet);
        printf("[OK] A2DP signaling connected (cid 0x%04x). "
               "Waiting for XM5 LDAC capability ...\n",
               app.a2dp_cid);
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
            printf("[..] vendor codec capability: remote_seid=%u — not LDAC, "
                   "ignoring\n", remote_seid);
            break;
        }

        uint8_t remote_sr_bitmap = info[6];
        uint8_t remote_cm_bitmap = info[7];
        uint8_t our_sr_bitmap    = ldac_caps[6];
        uint8_t our_cm_bitmap    = ldac_caps[7];
        uint8_t common_sr = remote_sr_bitmap & our_sr_bitmap;
        uint8_t common_cm = remote_cm_bitmap & our_cm_bitmap;

        printf("[OK] XM5 advertises LDAC: sr_bitmap=0x%02X cm_bitmap=0x%02X\n",
               remote_sr_bitmap, remote_cm_bitmap);
        printf("     intersection (us & xm5): sr=0x%02X cm=0x%02X\n",
               common_sr, common_cm);

        // M5: match WASAPI rate to avoid a resampler. M4 used "highest
        // common", which can pick 96 kHz while Windows is mixing at
        // 48 kHz; that's exactly the trap STATUS.md flagged.
        uint8_t chosen_sr =
            a2dp_ldac_pick_sample_rate_for_hz(common_sr, wasapi_sample_rate);
        uint8_t chosen_cm = a2dp_ldac_pick_best_channel_mode(common_cm);
        if (chosen_sr == 0 || chosen_cm == 0) {
            fprintf(stderr,
                "[ERR] No common sample rate or channel mode (sr=0x%02X cm=0x%02X)\n",
                common_sr, common_cm);
            shutdown_with_status(7);
            break;
        }
        if (a2dp_ldac_sf_bit_to_hz(chosen_sr) != wasapi_sample_rate) {
            // We didn't get the rate we wanted — XM5 doesn't support it.
            // Continue with the fallback but warn loudly; the user's
            // audio will sound at wrong pitch unless they change the
            // Windows mixer rate.
            fprintf(stderr,
                "[WARN] WASAPI is %d Hz but LDAC negotiated %s. Set Windows\n"
                "       Sound → device properties → Advanced format to that\n"
                "       rate to avoid pitched audio.\n",
                wasapi_sample_rate, a2dp_ldac_sf_label(chosen_sr));
        }
        printf("     chosen: %s, %s\n",
               a2dp_ldac_sf_label(chosen_sr),
               a2dp_ldac_cm_label(chosen_cm));

        a2dp_ldac_build_config(app.set_config_payload, chosen_sr, chosen_cm);
        app.negotiated_sr_hz  = a2dp_ldac_sf_bit_to_hz(chosen_sr);
        app.negotiated_cm_bit = chosen_cm;

        if (app.config_applied) break;
        app.config_applied = true;
        app.ldac_capability_seen = true;

        uint8_t rc = a2dp_source_set_config_other(
            app.a2dp_cid, app.local_seid, remote_seid,
            app.set_config_payload, sizeof(app.set_config_payload));
        if (rc != ERROR_CODE_SUCCESS) {
            fprintf(stderr,
                "[ERR] a2dp_source_set_config_other rc=0x%02x\n", rc);
            shutdown_with_status(8);
            break;
        }
        app.remote_seid = remote_seid;
        printf("[..] SET_CONFIGURATION queued. Waiting for XM5 ACCEPT ...\n");
        break;
    }

    case A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_OTHER_CONFIGURATION: {
        uint16_t info_len =
            a2dp_subevent_signaling_media_codec_other_configuration_get_media_codec_information_len(packet);
        const uint8_t* info =
            a2dp_subevent_signaling_media_codec_other_configuration_get_media_codec_information(packet);
        if (!a2dp_ldac_is_ldac_payload(info, info_len)) {
            fprintf(stderr,
                "[ERR] OTHER_CONFIGURATION not LDAC (len=%u)\n", info_len);
            shutdown_with_status(9);
            break;
        }
        printf("[OK] LDAC negotiated: %s, %s\n",
               a2dp_ldac_sf_label(info[6]),
               a2dp_ldac_cm_label(info[7]));
        // BTstack will move the AVDTP state machine forward and emit
        // STREAM_ESTABLISHED next.
        break;
    }

    case A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_SBC_CONFIGURATION:
        fprintf(stderr,
            "[ FAIL ] XM5 picked SBC despite our LDAC-only offer.\n");
        shutdown_with_status(10);
        break;

    case A2DP_SUBEVENT_STREAM_ESTABLISHED: {
        uint8_t status = a2dp_subevent_stream_established_get_status(packet);
        if (status != ERROR_CODE_SUCCESS) {
            fprintf(stderr,
                "[ERR] STREAM_ESTABLISHED status=0x%02x\n", status);
            shutdown_with_status(11);
            break;
        }
        app.stream_opened = true;
        printf("[OK] AVDTP stream open. Configuring LDAC encoder...\n");

        int rc = a2dp_ldac_source_setup(
            app.a2dp_cid, app.local_seid,
            app.negotiated_sr_hz,
            app.negotiated_cm_bit,
            A2DP_LDAC_BITRATE_FIXED_HQ,
            wasapi_loopback_read_f32);
        if (rc != 0) {
            fprintf(stderr, "[ERR] a2dp_ldac_source_setup failed\n");
            shutdown_with_status(12);
            break;
        }
        printf("[OK] LDAC encoder configured @ %d Hz, EQMID=HQ.\n",
               app.negotiated_sr_hz);
        printf("[..] a2dp_source_start_stream ...\n");
        a2dp_source_start_stream(app.a2dp_cid, app.local_seid);
        break;
    }

    case A2DP_SUBEVENT_STREAM_STARTED: {
        if (wasapi_loopback_start() != 0) {
            fprintf(stderr, "[ERR] wasapi_loopback_start failed\n");
            shutdown_with_status(13);
            break;
        }
        a2dp_ldac_source_start();
        app.streaming_started = true;
        btstack_run_loop_remove_timer(&neg_timeout_timer);

        printf("[OK] Streaming started. Play something on Windows now —\n"
               "    Spotify, B 站, anything.\n"
               "    Test runs for %d s; Ctrl+C to stop early.\n",
               TEST_DURATION_MS / 1000);

        btstack_run_loop_set_timer_handler(&total_duration_timer,
                                           &duration_handler);
        btstack_run_loop_set_timer(&total_duration_timer, TEST_DURATION_MS);
        btstack_run_loop_add_timer(&total_duration_timer);
        break;
    }

    case A2DP_SUBEVENT_STREAMING_CAN_SEND_MEDIA_PACKET_NOW:
        a2dp_ldac_source_on_can_send_now();
        break;

    case A2DP_SUBEVENT_STREAM_SUSPENDED:
        printf("[..] Stream suspended\n");
        a2dp_ldac_source_stop();
        break;

    case A2DP_SUBEVENT_STREAM_RELEASED:
        printf("[..] Stream released (underrun samples = %" PRIu64 ")\n",
               a2dp_ldac_source_underrun_samples());
        app.stream_opened = false;
        a2dp_ldac_source_stop();
        break;

    case A2DP_SUBEVENT_SIGNALING_CONNECTION_RELEASED:
        printf("[..] Signaling connection released\n");
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

    printf("a2dp_ldac_play_test - M5 acceptance\n");
    printf("===================================\n");

    // 1. Bring up WASAPI first so we know the system's mixer rate.
    int rc = wasapi_loopback_init(&wasapi_sample_rate, &wasapi_channels);
    if (rc != 0) {
        fprintf(stderr,
            "[ERR] wasapi_loopback_init returned %d.\n", rc);
        return 2;
    }
    printf("[OK] WASAPI loopback: %d Hz, %d channels (system mixer format)\n",
           wasapi_sample_rate, wasapi_channels);
    if (wasapi_channels != 2) {
        fprintf(stderr,
            "[ERR] Need stereo WASAPI loopback for M5 (got %d ch)\n",
            wasapi_channels);
        return 2;
    }
    if (!a2dp_ldac_hz_to_sf_bit(wasapi_sample_rate)) {
        fprintf(stderr,
            "[ERR] WASAPI mixer rate %d Hz isn't supported by LDAC.\n"
            "      Set Windows Sound → Properties → Advanced to\n"
            "      16-bit/44100 or 16-bit/48000.\n",
            wasapi_sample_rate);
        return 2;
    }

    sscanf_bd_addr(TARGET_DEVICE_ADDR, target_addr);

    btstack_memory_init();
    btstack_run_loop_init(btstack_run_loop_windows_get_instance());
    hci_init(hci_transport_usb_instance(), NULL);
    // Swap BTstack's default in-memory link key DB for our file-backed
    // one. Lets us survive a reboot without re-pairing the XM5.
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

    // Advertise full LDAC sample-rate support; STEREO only for now.
    a2dp_ldac_build_capability(ldac_caps, A2DP_LDAC_SF_ALL, A2DP_LDAC_CM_STEREO);

    avdtp_stream_endpoint_t* ldac_se = a2dp_source_create_stream_endpoint(
        AVDTP_AUDIO, AVDTP_CODEC_NON_A2DP,
        ldac_caps, sizeof(ldac_caps),
        ldac_config_buf, sizeof(ldac_config_buf));
    if (!ldac_se) {
        fprintf(stderr, "[ERR] could not create LDAC SEP\n");
        wasapi_loopback_stop();
        return 2;
    }
    app.local_seid = avdtp_local_seid(ldac_se);
    printf("[..] Registered LDAC stream endpoint, local_seid=%u\n",
           app.local_seid);

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

    btstack_run_loop_set_timer_handler(&neg_timeout_timer,
                                       &neg_timeout_handler);
    btstack_run_loop_set_timer(&neg_timeout_timer, NEGOTIATION_TIMEOUT_MS);
    btstack_run_loop_add_timer(&neg_timeout_timer);

    hci_power_control(HCI_POWER_ON);
    btstack_run_loop_execute();
    return 0;
}
