// engine_persistent_test — exercise the core_engine library from a
// console host. Same observable behaviour as a2dp_ldac_persistent_test
// (M6.2): connect, auto-reconnect, stream LDAC indefinitely. Difference
// is structural — all the BTstack/WASAPI/A2DP plumbing lives behind the
// engine_* API, leaving this tool ~50 lines.
//
// Once the GUI lands (M7 phase B), main.cpp / WinMain will spin up an
// ImGui window and call exactly the same engine_* API. This file is
// the CLI alternative.

#include <stdio.h>
#include <stdlib.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "btstack_run_loop.h"
#include "btstack_util.h"
#include "engine/engine.h"

// Hard-coded for M7 phase A; M8 will read this from a config file
// populated by the pairing flow.
#define TARGET_DEVICE_ADDR "88:C9:E8:F7:D5:F3"
#define LOCAL_NAME         "win-ldac (M7 CLI)"

// Periodically print effective bitrate + state so the user can see
// reconnects and ABR behaviour without a GUI.
static btstack_timer_source_t status_timer;
static void status_timer_handler(btstack_timer_source_t* ts) {
    btstack_run_loop_set_timer(ts, 1000);
    btstack_run_loop_add_timer(ts);

    engine_status_t st;
    engine_get_status_snapshot(&st);

    const char* state_label;
    switch (st.state) {
        case ENGINE_STATE_IDLE:         state_label = "idle";         break;
        case ENGINE_STATE_HCI_INIT:     state_label = "hci-init";     break;
        case ENGINE_STATE_DISCONNECTED: state_label = "disconnected"; break;
        case ENGINE_STATE_CONNECTING:   state_label = "connecting";   break;
        case ENGINE_STATE_NEGOTIATING:  state_label = "negotiating";  break;
        case ENGINE_STATE_STREAMING:    state_label = "streaming";    break;
        case ENGINE_STATE_STOPPING:     state_label = "stopping";     break;
        default:                        state_label = "?";            break;
    }
    if (st.state == ENGINE_STATE_STREAMING) {
        printf("[status] %s  %d Hz/%d-bit  EQMID=%d  nom=%d kbps  eff=%d kbps\n",
               state_label,
               st.sample_rate_hz, st.wasapi_bit_depth,
               st.eqmid, st.nominal_kbps, st.effective_kbps);
    } else {
        printf("[status] %s\n", state_label);
    }
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    setvbuf(stdout, NULL, _IONBF, 0);
    SetConsoleOutputCP(CP_UTF8);

    printf("engine_persistent_test - M7 phase A acceptance\n");
    printf("===============================================\n");
    printf("Target: %s\n\n", TARGET_DEVICE_ADDR);

    engine_config_t cfg;
    cfg.local_name             = LOCAL_NAME;
    cfg.reconnect_interval_ms  = 5000;
    cfg.initial_bitrate_mode   = ENGINE_BITRATE_FIXED_HQ;
    sscanf_bd_addr(TARGET_DEVICE_ADDR, cfg.target_addr);

    if (engine_init(&cfg) != 0) {
        fprintf(stderr, "engine_init failed\n");
        return 2;
    }

    btstack_run_loop_set_timer_handler(&status_timer, &status_timer_handler);
    btstack_run_loop_set_timer(&status_timer, 1000);
    btstack_run_loop_add_timer(&status_timer);

    engine_run();
    engine_shutdown();
    return 0;
}
