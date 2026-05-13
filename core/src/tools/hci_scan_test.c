// hci_scan_test — M2 acceptance test.
//
// Powers up BTstack on the windows-winusb transport, runs a 10-second GAP
// Classic Inquiry, requests remote names, and prints every device found.
//
// Acceptance: with a Sony WH-1000XM5 in pairing mode (long-press the right
// power button ~7 s, blue LED flashes), the output should include a line:
//
//     ●  AC:80:0A:XX:XX:XX  RSSI=-NN dBm  CoD=0x240404  "WH-1000XM5"
//
// Smoke-test (no dongle): the program should print
// "BTSTACK_EVENT_POWERON_FAILED" or exit with a transport error code,
// rather than crash.
//
// This file uses BTstack public APIs (btstack_*, hci_*, gap_*) and is
// structurally similar to BTstack's example/gap_inquiry.c. It is written
// from scratch against the BTstack public headers but follows the same
// state-machine pattern as that example. Distribution of a compiled
// binary is restricted by BTstack's non-commercial license; see
// HANDOFF.md §9.

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

// RTL8761BU canonical reference Product ID — used to select the right
// firmware blobs even when the actual USB device is a TP-Link UB4A
// (VID:PID 2357:0604) which carries the same silicon but a re-branded
// USB descriptor that isn't in BTstack's lookup table.
#define RTL8761BU_CANONICAL_PID 0x8771

#define MAX_DEVICES         32
#define INQUIRY_DURATION    8   /* 8 * 1.28 s ≈ 10.24 s */
#define HARD_TIMEOUT_MS     30000
#define LAP_GENERAL_INQUIRY 0x9E8B33UL

typedef enum {
    DEV_PENDING_NAME,
    DEV_REQUESTING_NAME,
    DEV_DONE
} dev_state_t;

typedef struct {
    bd_addr_t   addr;
    uint8_t     page_scan_repetition_mode;
    uint16_t    clock_offset;
    uint32_t    class_of_device;
    int8_t      rssi;
    bool        have_rssi;
    char        name[240];
    dev_state_t state;
} dev_entry_t;

static dev_entry_t devices[MAX_DEVICES];
static int         device_count = 0;
static bool        inquiry_complete = false;

static btstack_packet_callback_registration_t hci_event_callback_registration;
static btstack_timer_source_t                 hard_timeout_timer;

static int find_device(const bd_addr_t addr) {
    for (int i = 0; i < device_count; ++i) {
        if (bd_addr_cmp(devices[i].addr, addr) == 0) return i;
    }
    return -1;
}

static dev_entry_t* upsert_device(const bd_addr_t addr) {
    int i = find_device(addr);
    if (i >= 0) return &devices[i];
    if (device_count >= MAX_DEVICES) return NULL;
    dev_entry_t* d = &devices[device_count++];
    memset(d, 0, sizeof(*d));
    bd_addr_copy(d->addr, addr);
    d->state = DEV_PENDING_NAME;
    return d;
}

static void print_summary(void) {
    printf("\n────────────────────────────────────────────────\n");
    printf("Inquiry complete. %d device(s) found:\n", device_count);
    printf("────────────────────────────────────────────────\n");
    if (device_count == 0) {
        printf("  (no devices discovered — make sure XM5 is in pairing mode)\n");
        return;
    }
    bool xm5_found = false;
    for (int i = 0; i < device_count; ++i) {
        dev_entry_t* d = &devices[i];
        const char* name = d->name[0] ? d->name : "(name unavailable)";
        printf("  %s  %s  CoD=0x%06X  RSSI=%+4d dBm  \"%s\"\n",
               (strstr(name, "WH-1000XM5") || strstr(name, "WF-")) ? "●" : "·",
               bd_addr_to_str(d->addr),
               d->class_of_device,
               d->have_rssi ? d->rssi : 0,
               name);
        if (strstr(name, "WH-1000XM5")) xm5_found = true;
    }
    printf("────────────────────────────────────────────────\n");
    if (xm5_found) {
        printf("[ PASS ]  WH-1000XM5 discovered. M2 acceptance met.\n");
    } else {
        printf("[ INFO ]  XM5 not found in this scan. Re-run with the\n"
               "          headphone in pairing mode (right button 7s).\n");
    }
}

static void try_next_name_request(void) {
    for (int i = 0; i < device_count; ++i) {
        if (devices[i].state == DEV_PENDING_NAME) {
            devices[i].state = DEV_REQUESTING_NAME;
            gap_remote_name_request(
                devices[i].addr,
                devices[i].page_scan_repetition_mode,
                devices[i].clock_offset | 0x8000);
            return;
        }
    }
    // Nothing pending — if inquiry is also complete, we're done.
    if (inquiry_complete) {
        print_summary();
        hci_power_control(HCI_POWER_OFF);
    }
}

static void hard_timeout_handler(btstack_timer_source_t* ts) {
    (void)ts;
    fprintf(stderr, "\n[ TIMEOUT ]  hard 30 s limit reached, shutting down.\n");
    print_summary();
    hci_power_control(HCI_POWER_OFF);
}

static void packet_handler(uint8_t packet_type, uint16_t channel,
                           uint8_t* packet, uint16_t size) {
    (void)channel;
    (void)size;
    if (packet_type != HCI_EVENT_PACKET) return;

    uint8_t event = hci_event_packet_get_type(packet);
    switch (event) {

    case BTSTACK_EVENT_STATE: {
        uint8_t state = btstack_event_state_get_state(packet);
        if (state == HCI_STATE_WORKING) {
            bd_addr_t local;
            gap_local_bd_addr(local);
            printf("[OK] HCI up. Local BD_ADDR = %s\n", bd_addr_to_str(local));
            printf("[..] Starting %d.%d s Classic inquiry...\n\n",
                   (INQUIRY_DURATION * 128) / 100,
                   (INQUIRY_DURATION * 128) % 100);
            gap_inquiry_start(INQUIRY_DURATION);
        } else if (state == HCI_STATE_OFF) {
            printf("HCI off. Exiting.\n");
            exit(device_count > 0 ? 0 : 2);
        }
        break;
    }

    case BTSTACK_EVENT_POWERON_FAILED:
        fprintf(stderr,
                "[ERR] Power on failed. Likely causes:\n"
                "      - No USB Bluetooth dongle plugged in\n"
                "      - Dongle's driver is still Microsoft Bluetooth (run Zadig → WinUSB)\n"
                "      - Memory Integrity is ON (turn it off in Windows Security)\n");
        exit(3);

    case GAP_EVENT_INQUIRY_RESULT: {
        bd_addr_t addr;
        gap_event_inquiry_result_get_bd_addr(packet, addr);
        dev_entry_t* d = upsert_device(addr);
        if (!d) break;
        d->page_scan_repetition_mode =
            gap_event_inquiry_result_get_page_scan_repetition_mode(packet);
        d->clock_offset =
            gap_event_inquiry_result_get_clock_offset(packet);
        d->class_of_device =
            gap_event_inquiry_result_get_class_of_device(packet);
        if (gap_event_inquiry_result_get_rssi_available(packet)) {
            d->rssi      = (int8_t)gap_event_inquiry_result_get_rssi(packet);
            d->have_rssi = true;
        }
        if (gap_event_inquiry_result_get_name_available(packet)) {
            int len = gap_event_inquiry_result_get_name_len(packet);
            if (len >= (int)sizeof(d->name)) len = sizeof(d->name) - 1;
            memcpy(d->name, gap_event_inquiry_result_get_name(packet), len);
            d->name[len] = 0;
            d->state = DEV_DONE;
        }
        printf("  found %s  CoD=0x%06X%s\n",
               bd_addr_to_str(d->addr),
               d->class_of_device,
               d->name[0] ? "  (name in inquiry response)" : "");
        break;
    }

    case GAP_EVENT_INQUIRY_COMPLETE:
        printf("\n[OK] Inquiry complete. Requesting remote names...\n");
        inquiry_complete = true;
        try_next_name_request();
        break;

    case HCI_EVENT_REMOTE_NAME_REQUEST_COMPLETE: {
        bd_addr_t addr;
        reverse_bd_addr(&packet[3], addr);
        int i = find_device(addr);
        if (i < 0) break;
        uint8_t status = packet[2];
        if (status == 0) {
            const char* n = (const char*)&packet[9];
            size_t len = strnlen(n, sizeof(devices[i].name) - 1);
            memcpy(devices[i].name, n, len);
            devices[i].name[len] = 0;
            printf("  name  %s = \"%s\"\n",
                   bd_addr_to_str(addr), devices[i].name);
        } else {
            printf("  name  %s = (request failed, status=0x%02X)\n",
                   bd_addr_to_str(addr), status);
        }
        devices[i].state = DEV_DONE;
        try_next_name_request();
        break;
    }

    default:
        break;
    }
}

// Resolve the path to the firmware folder. Looks for "firmware" next to
// the executable. Writes a NUL-terminated absolute path into `out`.
// Returns true if the folder exists.
static bool resolve_firmware_folder(char* out, size_t out_sz) {
    char exe_path[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, exe_path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return false;
    // Strip filename to get directory.
    char* last_sep = strrchr(exe_path, '\\');
    if (!last_sep) return false;
    *last_sep = '\0';

    // Walk up from build/core/Release/ to project root looking for firmware/
    char candidate[MAX_PATH];
    char base[MAX_PATH];
    btstack_strcpy(base, sizeof(base), exe_path);
    for (int i = 0; i < 5; ++i) {
        btstack_snprintf_assert_complete(candidate, sizeof(candidate),
                                         "%s\\firmware", base);
        DWORD attr = GetFileAttributesA(candidate);
        if (attr != INVALID_FILE_ATTRIBUTES &&
            (attr & FILE_ATTRIBUTE_DIRECTORY)) {
            btstack_strcpy(out, out_sz, candidate);
            return true;
        }
        char* sep = strrchr(base, '\\');
        if (!sep) break;
        *sep = '\0';
    }
    return false;
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    setvbuf(stdout, NULL, _IONBF, 0);
    // Source is UTF-8 and we print Unicode box-drawing / dots. Tell the
    // console to interpret as UTF-8 so Chinese-Windows codepage 936 (GBK)
    // doesn't garble the output.
    SetConsoleOutputCP(CP_UTF8);

    printf("hci_scan_test — M2 smoke + acceptance\n");
    printf("──────────────────────────────────────\n");

    btstack_memory_init();
    btstack_run_loop_init(btstack_run_loop_windows_get_instance());

    // hci_init must come BEFORE chipset setup — the chipset driver
    // installs its own event handler during set_chipset, and that needs
    // HCI internals to already be initialised. Mirrors the order used by
    // BTstack's own libusb port (port/libusb/main.c).
    hci_init(hci_transport_usb_instance(), NULL);

    hci_event_callback_registration.callback = &packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);

    // ── Realtek RTL8761B(U) firmware loading ────────────────────────
    // The TP-Link UB4A / UB400v2 / UB500 / many Orico dongles all use
    // RTL8761B silicon and need firmware pushed via HCI vendor commands
    // on every power-on. BTstack's chipset driver handles the protocol;
    // we just need to point it at the .bin files and tell it which chip.
    char fw_dir[MAX_PATH];
    if (resolve_firmware_folder(fw_dir, sizeof(fw_dir))) {
        printf("[..] Realtek firmware folder: %s\n", fw_dir);
        btstack_chipset_realtek_set_firmware_folder_path(fw_dir);
        btstack_chipset_realtek_set_config_folder_path(fw_dir);
        btstack_chipset_realtek_set_product_id(RTL8761BU_CANONICAL_PID);
        hci_set_chipset(btstack_chipset_realtek_instance());
        hci_enable_custom_pre_init();
    } else {
        fprintf(stderr,
                "[WARN] firmware/ folder not found near executable.\n"
                "       Realtek dongles need rtl8761bu_fw.bin + rtl8761bu_config.bin.\n"
                "       This is fine if you have a CSR8510 (zero-firmware) dongle.\n");
    }

    // Hard timeout so the test always terminates.
    btstack_run_loop_set_timer_handler(&hard_timeout_timer, &hard_timeout_handler);
    btstack_run_loop_set_timer(&hard_timeout_timer, HARD_TIMEOUT_MS);
    btstack_run_loop_add_timer(&hard_timeout_timer);

    hci_power_control(HCI_POWER_ON);
    btstack_run_loop_execute();
    return 0;
}
