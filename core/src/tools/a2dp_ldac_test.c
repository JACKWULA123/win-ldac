// a2dp_ldac_test — M4 acceptance test.
//
// Negotiates LDAC over A2DP with a remote sink (the XM5 captured in M2).
// Does NOT stream audio — M5 will add the libldac encoder integration.
// Acceptance: see "AVDTP vendor codec config received" with Sony vendor
// ID 0x012D and codec ID 0x00AA in our stdout.
//
// Strategy: register ONLY an LDAC stream endpoint (no SBC). BTstack's
// default codec auto-selection only fires for SBC; with no local SBC
// SEP registered, the auto-path is harmless and we must explicitly
// drive a vendor config via a2dp_source_set_config_other() during the
// remote-capability phase.

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

// ── Configuration ──────────────────────────────────────────────────────
#define TARGET_DEVICE_ADDR  "88:C9:E8:F7:D5:F3"
#define LOCAL_NAME          "win-ldac (M4)"
#define CLASS_OF_DEVICE     0x200408
#define RTL8761BU_CANONICAL_PID 0x8771
#define NEGOTIATION_TIMEOUT_MS 15000

// LDAC vendor codec IDs (from AOSP libldac's ldacBT.h §"Codec Specific
// Information Elements for LDAC"):
#define LDAC_VENDOR_ID  0x0000012Du   // Sony Corporation
#define LDAC_CODEC_ID   0x00AAu       // LDAC

// Sampling-frequency support bitmap (capability advertising):
#define LDAC_SF_44100   0x20
#define LDAC_SF_48000   0x10
#define LDAC_SF_88200   0x08
#define LDAC_SF_96000   0x04
// Channel-mode support bitmap:
#define LDAC_CM_MONO    0x04
#define LDAC_CM_DUAL    0x02
#define LDAC_CM_STEREO  0x01

// Our advertised LDAC capability:
//   - vendor + codec IDs little-endian
//   - sample rates we accept: 44.1, 48, 88.2, 96 (all of LDAC's set)
//   - channel modes: STEREO only (M4 keeps it simple; mono/dual are oddities)
static const uint8_t ldac_capabilities[] = {
    0x2D, 0x01, 0x00, 0x00,                                    // vendor 0x012D
    0xAA, 0x00,                                                // codec  0x00AA
    LDAC_SF_44100 | LDAC_SF_48000 | LDAC_SF_88200 | LDAC_SF_96000,
    LDAC_CM_STEREO,
};
// Storage for the negotiated configuration BTstack reports back.
static uint8_t ldac_configuration_buf[sizeof(ldac_capabilities)];

static uint8_t sdp_a2dp_source_buf[150];
static uint8_t sdp_device_id_buf[100];

// ── State ──────────────────────────────────────────────────────────────
typedef struct {
    uint16_t a2dp_cid;
    uint8_t  local_seid;
    bool     ldac_capability_seen;
    bool     config_applied;
    bool     pass_done;
    // Selected LDAC config bytes. MUST have static lifetime — BTstack's
    // a2dp_source_set_config_other stashes the *pointer*, not the bytes,
    // and only sends AVDTP SET_CONFIGURATION later (after CAPABILITIES
    // _COMPLETE). A stack-local would be dangling by then; XM5 then sees
    // garbage and silently drops the request.
    uint8_t  set_config_payload[8];
} state_t;
static state_t s;

static bd_addr_t target_addr;
static btstack_packet_callback_registration_t hci_event_callback_registration;
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

static const char* sample_rate_label(uint8_t bit) {
    switch (bit) {
        case LDAC_SF_44100: return "44100 Hz";
        case LDAC_SF_48000: return "48000 Hz";
        case LDAC_SF_88200: return "88200 Hz";
        case LDAC_SF_96000: return "96000 Hz";
        default:            return "(unknown)";
    }
}

static const char* channel_mode_label(uint8_t bit) {
    switch (bit) {
        case LDAC_CM_STEREO: return "STEREO";
        case LDAC_CM_DUAL:   return "DUAL_CHANNEL";
        case LDAC_CM_MONO:   return "MONO";
        default:             return "(unknown)";
    }
}

// Pick the highest sample rate from `bitmap` that we and the remote both
// support. Bit values get *smaller* as the rate goes up (96k = 0x04,
// 44.1k = 0x20), so scan in order 0x04 → 0x20.
static uint8_t pick_best_sample_rate(uint8_t bitmap) {
    static const uint8_t prefer[] = {
        LDAC_SF_96000, LDAC_SF_88200, LDAC_SF_48000, LDAC_SF_44100
    };
    for (size_t i = 0; i < sizeof(prefer); ++i) {
        if (bitmap & prefer[i]) return prefer[i];
    }
    return 0;
}

static uint8_t pick_best_channel_mode(uint8_t bitmap) {
    if (bitmap & LDAC_CM_STEREO) return LDAC_CM_STEREO;
    if (bitmap & LDAC_CM_DUAL)   return LDAC_CM_DUAL;
    if (bitmap & LDAC_CM_MONO)   return LDAC_CM_MONO;
    return 0;
}

static void shutdown_with_status(int rc) {
    s.pass_done = (rc == 0);
    btstack_run_loop_remove_timer(&neg_timeout_timer);
    hci_power_control(HCI_POWER_OFF);
}

// ── Event handlers ─────────────────────────────────────────────────────
static void neg_timeout_handler(btstack_timer_source_t* ts) {
    (void)ts;
    fprintf(stderr,
        "\n[ FAIL ] negotiation timeout — XM5 didn't advertise LDAC, or\n"
        "         the AVDTP capability exchange didn't complete in %d s.\n",
        NEGOTIATION_TIMEOUT_MS / 1000);
    shutdown_with_status(2);
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
            uint8_t rc = a2dp_source_establish_stream(target_addr, &s.a2dp_cid);
            if (rc != ERROR_CODE_SUCCESS) {
                fprintf(stderr, "[ERR] establish_stream rc=0x%02x\n", rc);
                shutdown_with_status(4);
            }
        } else if (st == HCI_STATE_OFF) {
            printf("HCI off. %s.\n", s.pass_done ? "Bye" : "Exiting with failure");
            exit(s.pass_done ? 0 : 5);
        }
        break;
    }
    case BTSTACK_EVENT_POWERON_FAILED:
        fprintf(stderr, "[ERR] Power on failed.\n");
        exit(3);
    case HCI_EVENT_USER_CONFIRMATION_REQUEST: {
        bd_addr_t addr;
        hci_event_user_confirmation_request_get_bd_addr(packet, addr);
        printf("[..] SSP from %s — auto-accept\n", bd_addr_to_str(addr));
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
                "[ERR] A2DP signaling connect failed status=0x%02x\n", status);
            shutdown_with_status(6);
            break;
        }
        s.a2dp_cid =
            a2dp_subevent_signaling_connection_established_get_a2dp_cid(packet);
        printf("[OK] A2DP signaling connected (cid 0x%04x). "
               "Waiting for XM5 to advertise LDAC capability ...\n",
               s.a2dp_cid);
        break;
    }

    // XM5 reports each of its sink stream endpoints' capabilities. Vendor
    // codecs come in via OTHER_CAPABILITY. If we recognise LDAC, drive
    // SET_CONFIGURATION ourselves (BTstack's auto-pick path only covers
    // SBC).
    case A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_OTHER_CAPABILITY: {
        uint8_t remote_seid =
            a2dp_subevent_signaling_media_codec_other_capability_get_remote_seid(packet);
        uint16_t info_len =
            a2dp_subevent_signaling_media_codec_other_capability_get_media_codec_information_len(packet);
        const uint8_t* info =
            a2dp_subevent_signaling_media_codec_other_capability_get_media_codec_information(packet);

        if (info_len < 8) {
            printf("[..] vendor codec capability: remote_seid=%u len=%u — "
                   "too short to be LDAC\n", remote_seid, info_len);
            break;
        }
        uint32_t vendor =
            (uint32_t)info[0] |
            ((uint32_t)info[1] << 8) |
            ((uint32_t)info[2] << 16) |
            ((uint32_t)info[3] << 24);
        uint16_t codec = (uint16_t)info[4] | ((uint16_t)info[5] << 8);
        if (vendor != LDAC_VENDOR_ID || codec != LDAC_CODEC_ID) {
            printf("[..] vendor codec capability: remote_seid=%u "
                   "vendor=0x%08X codec=0x%04X — not LDAC, ignoring\n",
                   remote_seid, vendor, codec);
            break;
        }

        // LDAC capability from XM5. Bytes 6 and 7 are the sample-rate
        // and channel-mode support bitmaps.
        uint8_t remote_sr_bitmap = info[6];
        uint8_t remote_cm_bitmap = info[7];
        uint8_t our_sr_bitmap = ldac_capabilities[6];
        uint8_t our_cm_bitmap = ldac_capabilities[7];

        uint8_t chosen_sr = pick_best_sample_rate(remote_sr_bitmap & our_sr_bitmap);
        uint8_t chosen_cm = pick_best_channel_mode(remote_cm_bitmap & our_cm_bitmap);

        printf("[OK] XM5 advertises LDAC: sr_bitmap=0x%02X cm_bitmap=0x%02X\n",
               remote_sr_bitmap, remote_cm_bitmap);
        printf("     intersection (us & xm5): sr=0x%02X cm=0x%02X\n",
               remote_sr_bitmap & our_sr_bitmap,
               remote_cm_bitmap & our_cm_bitmap);
        if (chosen_sr == 0 || chosen_cm == 0) {
            fprintf(stderr,
                "[ERR] No common sample rate or channel mode.\n");
            shutdown_with_status(7);
            break;
        }
        printf("     chosen: %s, %s\n",
               sample_rate_label(chosen_sr), channel_mode_label(chosen_cm));

        // Build the 8-byte SET_CONFIGURATION payload in a buffer with
        // static lifetime (see comment on state_t::set_config_payload).
        memcpy(s.set_config_payload, info, 6);  // vendor + codec IDs
        s.set_config_payload[6] = chosen_sr;
        s.set_config_payload[7] = chosen_cm;

        if (s.config_applied) break;  // ignore stale OTHER_CAPABILITY repeats
        s.config_applied = true;
        s.ldac_capability_seen = true;

        uint8_t rc = a2dp_source_set_config_other(
            s.a2dp_cid, s.local_seid, remote_seid,
            s.set_config_payload, sizeof(s.set_config_payload));
        if (rc != ERROR_CODE_SUCCESS) {
            fprintf(stderr,
                "[ERR] a2dp_source_set_config_other rc=0x%02x\n", rc);
            shutdown_with_status(8);
            break;
        }
        printf("[..] SET_CONFIGURATION sent for our LDAC SEP. "
               "Awaiting XM5 ACCEPT ...\n");
        break;
    }

    // Sink (XM5) ACCEPTed our SET_CONFIGURATION. M4 acceptance point.
    case A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_OTHER_CONFIGURATION: {
        uint16_t info_len =
            a2dp_subevent_signaling_media_codec_other_configuration_get_media_codec_information_len(packet);
        const uint8_t* info =
            a2dp_subevent_signaling_media_codec_other_configuration_get_media_codec_information(packet);
        if (info_len < 8) {
            fprintf(stderr,
                "[ERR] CONFIGURATION event payload too short (%u)\n", info_len);
            shutdown_with_status(9);
            break;
        }
        uint32_t vendor =
            (uint32_t)info[0] | ((uint32_t)info[1] << 8) |
            ((uint32_t)info[2] << 16) | ((uint32_t)info[3] << 24);
        uint16_t codec = (uint16_t)info[4] | ((uint16_t)info[5] << 8);

        printf("\n[OK] AVDTP vendor codec configuration negotiated:\n");
        printf("       vendor_id    = 0x%08X  %s\n", vendor,
               (vendor == LDAC_VENDOR_ID) ? "(Sony)" : "(?)");
        printf("       codec_id     = 0x%04X      %s\n", codec,
               (codec == LDAC_CODEC_ID) ? "(LDAC)" : "(?)");
        printf("       sample_rate  = %s\n",   sample_rate_label(info[6]));
        printf("       channel_mode = %s\n\n", channel_mode_label(info[7]));

        if (vendor == LDAC_VENDOR_ID && codec == LDAC_CODEC_ID) {
            printf("[ PASS ]  M4 acceptance met — LDAC selected by XM5.\n");
            printf("          (M5 will hook in the libldac encoder; for now\n"
                   "           we hang up without streaming.)\n");
            shutdown_with_status(0);
        } else {
            fprintf(stderr,
                "[ FAIL ]  Wrong codec negotiated.\n");
            shutdown_with_status(10);
        }
        break;
    }

    // SBC fallback path — should not happen, since we don't register
    // an SBC SEP. If we ever see it, our config selection has a bug.
    case A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_SBC_CONFIGURATION:
        fprintf(stderr,
            "[ FAIL ]  XM5 picked SBC despite our LDAC-only offer.\n"
            "          Either XM5 doesn't actually support LDAC on this\n"
            "          firmware, or our capability bytes are wrong.\n");
        shutdown_with_status(11);
        break;

    case A2DP_SUBEVENT_SIGNALING_CAPABILITIES_COMPLETE:
        if (!s.ldac_capability_seen) {
            fprintf(stderr,
                "[ FAIL ]  XM5 finished advertising capabilities but never\n"
                "          offered an LDAC vendor codec SEP.\n");
            shutdown_with_status(12);
        }
        break;

    case A2DP_SUBEVENT_SIGNALING_CONNECTION_RELEASED:
        printf("[..] Signaling connection released.\n");
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

    printf("a2dp_ldac_test - M4 acceptance (negotiation only, no audio)\n");
    printf("===========================================================\n");
    printf("Target: XM5 at %s\n", TARGET_DEVICE_ADDR);

    sscanf_bd_addr(TARGET_DEVICE_ADDR, target_addr);

    btstack_memory_init();
    btstack_run_loop_init(btstack_run_loop_windows_get_instance());
    hci_init(hci_transport_usb_instance(), NULL);
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

    // Register ONLY the LDAC stream endpoint — no SBC SEP. With no local
    // SBC SEP, BTstack's automatic codec-pick path (which only handles
    // SBC) has nothing to do, and our explicit a2dp_source_set_config_other
    // call drives the negotiation.
    avdtp_stream_endpoint_t* ldac_se = a2dp_source_create_stream_endpoint(
        AVDTP_AUDIO, AVDTP_CODEC_NON_A2DP,
        ldac_capabilities, sizeof(ldac_capabilities),
        ldac_configuration_buf, sizeof(ldac_configuration_buf));
    if (!ldac_se) {
        fprintf(stderr, "[ERR] could not create LDAC SEP\n");
        return 2;
    }
    s.local_seid = avdtp_local_seid(ldac_se);
    printf("[..] Registered LDAC stream endpoint, local_seid=%u\n",
           s.local_seid);

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

    btstack_run_loop_set_timer_handler(&neg_timeout_timer, &neg_timeout_handler);
    btstack_run_loop_set_timer(&neg_timeout_timer, NEGOTIATION_TIMEOUT_MS);
    btstack_run_loop_add_timer(&neg_timeout_timer);

    hci_power_control(HCI_POWER_ON);
    btstack_run_loop_execute();
    return 0;
}
