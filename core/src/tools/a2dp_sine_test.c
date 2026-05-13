// a2dp_sine_test — M3.1 acceptance test.
//
// Initialises BTstack as an A2DP Source, connects to the hardcoded XM5
// BD_ADDR captured in M2, negotiates SBC, and streams a 441 Hz sine wave
// for 60 seconds. The acceptance criterion is hearing the tone on the
// headphone continuously.
//
// Structure heavily based on BTstack's example/a2dp_source_demo.c, with
// AVRCP / mod player / stdin command interface stripped out. Keep the
// pieces that matter for "audio actually flows".

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
#define TARGET_DEVICE_ADDR "88:C9:E8:F7:D5:F3"   // XM5 captured in M2
#define LOCAL_NAME         "win-ldac (M3.1)"
#define CLASS_OF_DEVICE    0x200408              // A/V → wearable headset? actually use loudspeaker
#define PREFERRED_SAMPLE_RATE 44100              // SBC sweet spot at 44.1
#define NUM_CHANNELS       2
#define AUDIO_TIMER_MS     10
#define SBC_STORAGE_SIZE   1030
#define RTL8761BU_CANONICAL_PID 0x8771
#define TEST_DURATION_MS   60000                 // play 60 seconds then quit

// ── State ──────────────────────────────────────────────────────────────
typedef struct {
    uint16_t a2dp_cid;
    uint8_t  local_seid;
    uint8_t  remote_seid;
    bool     stream_opened;

    uint32_t time_audio_data_sent_ms;
    uint32_t acc_missed_samples;
    uint32_t samples_ready;
    btstack_timer_source_t audio_timer;
    bool     streaming;
    int      max_media_payload_size;
    uint32_t rtp_timestamp;

    uint8_t  sbc_storage[SBC_STORAGE_SIZE];
    uint16_t sbc_storage_count;
    bool     sbc_ready_to_send;
} media_tracker_t;

static media_tracker_t media_tracker;

// SBC codec capabilities advertised to the sink. The middle byte 0xFF
// means "any block length / subbands / allocation method", letting the
// sink pick freely.
static uint8_t sbc_capabilities[] = {
    (AVDTP_SBC_44100 << 4) | (AVDTP_SBC_48000 << 4) | AVDTP_SBC_STEREO,
    0xFF,
    2, 53
};
static uint8_t sbc_configuration_buf[4];

// SDP service records
static uint8_t sdp_a2dp_source_buf[150];
static uint8_t sdp_device_id_buf[100];

// SBC encoder state
static const btstack_sbc_encoder_t * sbc_encoder_instance;
static btstack_sbc_encoder_bluedroid_t sbc_encoder_state;

// Audio generator (sine)
static struct {
    btstack_audio_generator_sine_t sine;
    bool initialized;
} audio_gen;

static int current_sample_rate = PREFERRED_SAMPLE_RATE;
static bd_addr_t target_addr;

static btstack_packet_callback_registration_t hci_event_callback_registration;
static btstack_timer_source_t total_duration_timer;

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
        char* s = strrchr(base, '\\');
        if (!s) break;
        *s = '\0';
    }
    return false;
}

// ── Audio production ───────────────────────────────────────────────────
static void produce_audio(int16_t* pcm, int num_samples) {
    btstack_audio_generator_generate(
        (btstack_audio_generator_t*)&audio_gen.sine, pcm, num_samples);
}

static void init_audio_generator(int sample_rate) {
    if (audio_gen.initialized) {
        btstack_audio_generator_finalize(
            (btstack_audio_generator_t*)&audio_gen.sine);
    }
    btstack_audio_generator_sine_init(&audio_gen.sine, sample_rate,
                                      NUM_CHANNELS, 441);
    audio_gen.initialized = true;
    media_tracker.sbc_storage_count = 0;
    media_tracker.samples_ready = 0;
}

static int fill_sbc_storage(void) {
    unsigned int samples_per_sbc_frame =
        sbc_encoder_instance->num_audio_frames(&sbc_encoder_state);
    uint16_t sbc_frame_size =
        sbc_encoder_instance->sbc_buffer_length(&sbc_encoder_state);
    int total_consumed = 0;

    while (media_tracker.samples_ready >= samples_per_sbc_frame &&
           (media_tracker.max_media_payload_size
                - media_tracker.sbc_storage_count) >= sbc_frame_size) {
        int16_t pcm_frame[256 * NUM_CHANNELS];
        produce_audio(pcm_frame, samples_per_sbc_frame);
        sbc_encoder_instance->encode_signed_16(
            &sbc_encoder_state, pcm_frame,
            &media_tracker.sbc_storage[1 + media_tracker.sbc_storage_count]);
        media_tracker.sbc_storage_count += sbc_frame_size;
        media_tracker.samples_ready     -= samples_per_sbc_frame;
        total_consumed                  += samples_per_sbc_frame;
    }
    return total_consumed;
}

static void send_media_packet(void) {
    uint16_t sbc_frame_size =
        sbc_encoder_instance->sbc_buffer_length(&sbc_encoder_state);
    int bytes_in_storage = media_tracker.sbc_storage_count;
    uint8_t num_sbc_frames = bytes_in_storage / sbc_frame_size;
    // AVDTP SBC media payload header (1 byte: F/S/L/num_frames).
    media_tracker.sbc_storage[0] = num_sbc_frames;
    a2dp_source_stream_send_media_payload_rtp(
        media_tracker.a2dp_cid, media_tracker.local_seid, 0,
        media_tracker.rtp_timestamp,
        media_tracker.sbc_storage, bytes_in_storage + 1);

    unsigned int samples_per_sbc_frame =
        sbc_encoder_instance->num_audio_frames(&sbc_encoder_state);
    media_tracker.rtp_timestamp += num_sbc_frames * samples_per_sbc_frame;

    media_tracker.sbc_storage_count = 0;
    media_tracker.sbc_ready_to_send = false;
}

static void audio_timer_handler(btstack_timer_source_t* ts) {
    btstack_run_loop_set_timer(ts, AUDIO_TIMER_MS);
    btstack_run_loop_add_timer(ts);
    uint32_t now = btstack_run_loop_get_time_ms();

    uint32_t period = AUDIO_TIMER_MS;
    if (media_tracker.time_audio_data_sent_ms != 0) {
        period = now - media_tracker.time_audio_data_sent_ms;
    }
    uint32_t num_samples = (period * current_sample_rate) / 1000;
    media_tracker.acc_missed_samples += (period * current_sample_rate) % 1000;
    while (media_tracker.acc_missed_samples >= 1000) {
        num_samples++;
        media_tracker.acc_missed_samples -= 1000;
    }
    media_tracker.time_audio_data_sent_ms = now;
    media_tracker.samples_ready          += num_samples;

    if (media_tracker.sbc_ready_to_send) return;

    fill_sbc_storage();

    uint16_t sbc_frame_size =
        sbc_encoder_instance->sbc_buffer_length(&sbc_encoder_state);
    if ((media_tracker.sbc_storage_count + sbc_frame_size)
            > (uint16_t)media_tracker.max_media_payload_size) {
        media_tracker.sbc_ready_to_send = true;
        a2dp_source_stream_endpoint_request_can_send_now(
            media_tracker.a2dp_cid, media_tracker.local_seid);
    }
}

static void audio_timer_start(void) {
    media_tracker.max_media_payload_size = btstack_min(
        a2dp_max_media_payload_size(media_tracker.a2dp_cid,
                                    media_tracker.local_seid),
        SBC_STORAGE_SIZE);
    media_tracker.sbc_storage_count = 0;
    media_tracker.sbc_ready_to_send = false;
    media_tracker.streaming         = true;
    btstack_run_loop_remove_timer(&media_tracker.audio_timer);
    btstack_run_loop_set_timer_handler(&media_tracker.audio_timer,
                                       &audio_timer_handler);
    btstack_run_loop_set_timer(&media_tracker.audio_timer, AUDIO_TIMER_MS);
    btstack_run_loop_add_timer(&media_tracker.audio_timer);
}

static void audio_timer_stop(void) {
    media_tracker.time_audio_data_sent_ms = 0;
    media_tracker.acc_missed_samples      = 0;
    media_tracker.samples_ready           = 0;
    media_tracker.sbc_storage_count       = 0;
    media_tracker.sbc_ready_to_send       = false;
    btstack_run_loop_remove_timer(&media_tracker.audio_timer);
}

// ── HCI event handler ──────────────────────────────────────────────────
static void hci_packet_handler(uint8_t pt, uint16_t ch,
                               uint8_t* packet, uint16_t size) {
    (void)ch; (void)size;
    if (pt != HCI_EVENT_PACKET) return;
    uint8_t evt = hci_event_packet_get_type(packet);

    switch (evt) {
    case BTSTACK_EVENT_STATE: {
        uint8_t s = btstack_event_state_get_state(packet);
        if (s == HCI_STATE_WORKING) {
            bd_addr_t local;
            gap_local_bd_addr(local);
            printf("[OK] HCI up. Local %s. Connecting to XM5 %s ...\n",
                   bd_addr_to_str(local), TARGET_DEVICE_ADDR);
            uint8_t rc = a2dp_source_establish_stream(target_addr,
                                                      &media_tracker.a2dp_cid);
            if (rc != ERROR_CODE_SUCCESS) {
                fprintf(stderr,
                        "[ERR] a2dp_source_establish_stream rc=0x%02x\n", rc);
                exit(4);
            }
        } else if (s == HCI_STATE_OFF) {
            printf("HCI off. Bye.\n");
            exit(0);
        }
        break;
    }
    case BTSTACK_EVENT_POWERON_FAILED:
        fprintf(stderr,
                "[ERR] Power on failed. Check Zadig + Memory Integrity.\n");
        exit(3);
    case HCI_EVENT_PIN_CODE_REQUEST: {
        bd_addr_t addr;
        hci_event_pin_code_request_get_bd_addr(packet, addr);
        printf("[..] Legacy PIN request from %s — replying '0000'\n",
               bd_addr_to_str(addr));
        gap_pin_code_response(addr, "0000");
        break;
    }
    case HCI_EVENT_USER_CONFIRMATION_REQUEST: {
        bd_addr_t addr;
        hci_event_user_confirmation_request_get_bd_addr(packet, addr);
        printf("[..] SSP user confirmation from %s — auto-accept\n",
               bd_addr_to_str(addr));
        // BTstack with gap_ssp_set_auto_accept(true) handles this for us,
        // but keep the log for visibility.
        break;
    }
    default:
        break;
    }
}

// ── A2DP source event handler ──────────────────────────────────────────
static void a2dp_source_handler(uint8_t pt, uint16_t ch,
                                uint8_t* packet, uint16_t size) {
    (void)ch; (void)size;
    if (pt != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != HCI_EVENT_A2DP_META) return;

    uint8_t sub = hci_event_a2dp_meta_get_subevent_code(packet);
    switch (sub) {

    case A2DP_SUBEVENT_SIGNALING_CONNECTION_ESTABLISHED: {
        bd_addr_t addr;
        a2dp_subevent_signaling_connection_established_get_bd_addr(packet, addr);
        uint16_t cid =
            a2dp_subevent_signaling_connection_established_get_a2dp_cid(packet);
        uint8_t status =
            a2dp_subevent_signaling_connection_established_get_status(packet);
        if (status != ERROR_CODE_SUCCESS) {
            fprintf(stderr,
                    "[ERR] A2DP signaling connect failed status=0x%02x\n",
                    status);
            media_tracker.a2dp_cid = 0;
            break;
        }
        media_tracker.a2dp_cid = cid;
        printf("[OK] A2DP signaling established with %s (cid 0x%04x)\n",
               bd_addr_to_str(addr), cid);
        break;
    }

    case A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_SBC_CONFIGURATION: {
        if (a2dp_subevent_signaling_media_codec_sbc_configuration_get_a2dp_cid(packet)
                != media_tracker.a2dp_cid) return;
        media_tracker.remote_seid =
            a2dp_subevent_signaling_media_codec_sbc_configuration_get_remote_seid(packet);

        int sr = a2dp_subevent_signaling_media_codec_sbc_configuration_get_sampling_frequency(packet);
        int ch_count = a2dp_subevent_signaling_media_codec_sbc_configuration_get_num_channels(packet);
        int block = a2dp_subevent_signaling_media_codec_sbc_configuration_get_block_length(packet);
        int sub_bands = a2dp_subevent_signaling_media_codec_sbc_configuration_get_subbands(packet);
        int max_bp = a2dp_subevent_signaling_media_codec_sbc_configuration_get_max_bitpool_value(packet);
        int alloc = a2dp_subevent_signaling_media_codec_sbc_configuration_get_allocation_method(packet);
        avdtp_channel_mode_t cm = (avdtp_channel_mode_t)
            a2dp_subevent_signaling_media_codec_sbc_configuration_get_channel_mode(packet);

        printf("[OK] SBC negotiated: %d Hz, %d ch, blocks=%d, subbands=%d, "
               "bitpool=%d, alloc=%d, ch_mode=%d\n",
               sr, ch_count, block, sub_bands, max_bp, alloc, cm);

        btstack_sbc_channel_mode_t sbc_cm;
        switch (cm) {
            case AVDTP_CHANNEL_MODE_JOINT_STEREO: sbc_cm = SBC_CHANNEL_MODE_JOINT_STEREO; break;
            case AVDTP_CHANNEL_MODE_STEREO:       sbc_cm = SBC_CHANNEL_MODE_STEREO;       break;
            case AVDTP_CHANNEL_MODE_DUAL_CHANNEL: sbc_cm = SBC_CHANNEL_MODE_DUAL_CHANNEL; break;
            case AVDTP_CHANNEL_MODE_MONO:         sbc_cm = SBC_CHANNEL_MODE_MONO;         break;
            default: sbc_cm = SBC_CHANNEL_MODE_STEREO; break;
        }
        current_sample_rate = sr;
        sbc_encoder_instance = btstack_sbc_encoder_bluedroid_init_instance(&sbc_encoder_state);
        sbc_encoder_instance->configure(&sbc_encoder_state, SBC_MODE_STANDARD,
                                        block, sub_bands,
                                        (btstack_sbc_allocation_method_t)(alloc - 1),
                                        sr, max_bp, sbc_cm);
        break;
    }

    case A2DP_SUBEVENT_STREAM_ESTABLISHED: {
        uint8_t status = a2dp_subevent_stream_established_get_status(packet);
        if (status != ERROR_CODE_SUCCESS) {
            fprintf(stderr,
                    "[ERR] A2DP stream establish failed status=0x%02x\n",
                    status);
            break;
        }
        media_tracker.stream_opened = true;
        printf("[OK] A2DP stream open; starting playback...\n");
        a2dp_source_start_stream(media_tracker.a2dp_cid,
                                  media_tracker.local_seid);
        break;
    }

    case A2DP_SUBEVENT_STREAM_STARTED:
        init_audio_generator(current_sample_rate);
        audio_timer_start();
        printf("[OK] Stream started — 441 Hz tone should be audible on XM5 "
               "for %d seconds.\n", TEST_DURATION_MS / 1000);
        break;

    case A2DP_SUBEVENT_STREAMING_CAN_SEND_MEDIA_PACKET_NOW:
        send_media_packet();
        break;

    case A2DP_SUBEVENT_STREAM_SUSPENDED:
        printf("[..] Stream suspended\n");
        audio_timer_stop();
        break;

    case A2DP_SUBEVENT_STREAM_RELEASED:
        printf("[..] Stream released\n");
        media_tracker.stream_opened = false;
        audio_timer_stop();
        break;

    case A2DP_SUBEVENT_SIGNALING_CONNECTION_RELEASED:
        printf("[..] Signaling connection released\n");
        break;

    default:
        break;
    }
}

// ── Test duration cap ──────────────────────────────────────────────────
static void duration_handler(btstack_timer_source_t* ts) {
    (void)ts;
    printf("\n[ PASS ]  M3.1 test duration elapsed. Shutting down.\n");
    audio_timer_stop();
    hci_power_control(HCI_POWER_OFF);
}

// ── Main ───────────────────────────────────────────────────────────────
int main(int argc, char** argv) {
    (void)argc; (void)argv;
    setvbuf(stdout, NULL, _IONBF, 0);
    SetConsoleOutputCP(CP_UTF8);

    printf("a2dp_sine_test - M3.1 acceptance\n");
    printf("================================\n");
    printf("Target: XM5 at %s\n", TARGET_DEVICE_ADDR);

    sscanf_bd_addr(TARGET_DEVICE_ADDR, target_addr);

    btstack_memory_init();
    btstack_run_loop_init(btstack_run_loop_windows_get_instance());

    hci_init(hci_transport_usb_instance(), NULL);

    hci_event_callback_registration.callback = &hci_packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);

    // Realtek firmware loading (same as M2 — required for RTL8761B dongles)
    char fw_dir[MAX_PATH];
    if (resolve_firmware_folder(fw_dir, sizeof(fw_dir))) {
        printf("[..] Realtek firmware folder: %s\n", fw_dir);
        btstack_chipset_realtek_set_firmware_folder_path(fw_dir);
        btstack_chipset_realtek_set_config_folder_path(fw_dir);
        btstack_chipset_realtek_set_product_id(RTL8761BU_CANONICAL_PID);
        hci_set_chipset(btstack_chipset_realtek_instance());
        hci_enable_custom_pre_init();
    }

    // Classic / GAP setup — XM5 is paired as master, we (PC) act as slave
    // for audio source role, but BR/EDR role policy lets the remote decide.
    hci_set_master_slave_policy(0);
    hci_set_inquiry_mode(INQUIRY_MODE_RSSI_AND_EIR);

    // SSP "Just Works" — XM5 has no display, no keyboard. Auto-accept.
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    gap_ssp_set_auto_accept(1);

    l2cap_init();

    // A2DP source profile + SBC stream endpoint
    a2dp_source_init();
    a2dp_source_register_packet_handler(&a2dp_source_handler);

    avdtp_stream_endpoint_t* local_se = a2dp_source_create_stream_endpoint(
        AVDTP_AUDIO, AVDTP_CODEC_SBC,
        sbc_capabilities, sizeof(sbc_capabilities),
        sbc_configuration_buf, sizeof(sbc_configuration_buf));
    if (!local_se) {
        fprintf(stderr, "[ERR] could not create A2DP stream endpoint\n");
        return 2;
    }
    avdtp_set_preferred_sampling_frequency(local_se, PREFERRED_SAMPLE_RATE);
    media_tracker.local_seid = avdtp_local_seid(local_se);
    avdtp_source_register_delay_reporting_category(media_tracker.local_seid);

    // SDP service records (XM5 may query us; we advertise A2DP source)
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

    // Hard test-duration timer so we always quit.
    btstack_run_loop_set_timer_handler(&total_duration_timer, &duration_handler);
    btstack_run_loop_set_timer(&total_duration_timer, TEST_DURATION_MS);
    btstack_run_loop_add_timer(&total_duration_timer);

    hci_power_control(HCI_POWER_ON);
    btstack_run_loop_execute();
    return 0;
}
