// engine — the headless "always trying to stream LDAC to the configured
// XM5" core. Pulled out of a2dp_ldac_persistent_test.c (M6.2) so that
// both the test CLI and the upcoming Dear ImGui GUI share one
// implementation.
//
// Threading model
// ---------------
// All state lives on the BTstack run-loop thread. The owner thread:
//   - calls engine_init()
//   - calls engine_run() — blocks on btstack_run_loop_execute() until
//     engine_request_stop() is invoked
//   - calls engine_shutdown() after run() returns
//
// Cross-thread access (e.g. from the GUI thread) goes through:
//   - engine_get_status_snapshot() — reads an atomically-updated
//     snapshot, no need to be on the BT thread
//   - engine_post_*() — async command APIs that internally use
//     btstack_run_loop_execute_on_main_thread to dispatch onto the BT
//     thread. Safe to call from anywhere.
//
// In Phase A there's no GUI yet, so the test tool just calls engine_*
// inline on the BT thread. The async APIs are still there so we don't
// have to re-shape the interface when the GUI lands.

#ifndef WIN_LDAC_ENGINE_H
#define WIN_LDAC_ENGINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bluetooth.h"  // bd_addr_t

#ifdef __cplusplus
extern "C" {
#endif

// ── Public state types ─────────────────────────────────────────────────

typedef enum {
    ENGINE_STATE_IDLE = 0,         // before engine_run() is called
    ENGINE_STATE_HCI_INIT,         // HCI powering on, waiting for HCI_STATE_WORKING
    ENGINE_STATE_DISCONNECTED,     // no link; reconnect supervisor may be retrying
    ENGINE_STATE_CONNECTING,       // a2dp_source_establish_stream in flight
    ENGINE_STATE_NEGOTIATING,      // signalling up, waiting for OTHER_CAPABILITY / STREAM_ESTABLISHED
    ENGINE_STATE_STREAMING,        // STREAM_STARTED, audio flowing
    ENGINE_STATE_STOPPING,         // engine_request_stop() received
} engine_state_t;

typedef enum {
    ENGINE_BITRATE_FIXED_HQ = 0,   // EQMID locked at HQ (990 kbps)
    ENGINE_BITRATE_ADAPTIVE,       // ldac_ABR_Proc drives EQMID from txq depth
} engine_bitrate_mode_t;

// Status snapshot, lock-free read for the GUI thread. Numbers are
// updated by the engine on its own thread; readers may see slightly
// stale values but never torn structs (we copy under a critical
// section internally).
typedef struct {
    engine_state_t        state;
    bd_addr_t             target_addr;       // configured target ({0} if unpaired)
    bool                  has_target;        // false → engine is idle, awaiting pairing
    bool                  link_up;
    int                   sample_rate_hz;    // negotiated LDAC sample rate
    int                   wasapi_bit_depth;  // 16 / 24 / 32 (from Windows device format)
    int                   effective_kbps;    // bytes_sent / time, smoothed
    int                   nominal_kbps;      // current EQMID's nominal kbps (990/660/330)
    int                   eqmid;             // 0=HQ, 1=SQ, 2=MQ (current)
    engine_bitrate_mode_t bitrate_mode;
    uint64_t              underrun_samples;
    int                   reconnect_attempts;
    bool                  idle_paused;       // AVDTP-suspended on silence; audio not flowing

    // Non-zero if the Windows audio mixer is currently at a sample rate
    // LDAC can't handle (anything except 44.1/48/88.2/96 kHz). Value is
    // the offending rate in Hz so the GUI can name it in its dialog.
    int                   wasapi_unsupported_rate_hz;
} engine_status_t;

// ── Scan state (M8 pairing flow) ───────────────────────────────────────
// A separate snapshot the GUI polls while the pair modal is open.
// Independent of engine_status_t so updating one doesn't churn the other.

typedef struct {
    bd_addr_t addr;
    char      name[64];        // empty if name not yet resolved
    int8_t    rssi_dbm;        // 0 if unknown
    uint32_t  cod;             // Class of Device (24-bit)
    bool      have_name;
    bool      have_rssi;
} engine_scan_device_t;

#define ENGINE_SCAN_MAX 24

typedef struct {
    bool                 active;          // inquiry running or names being resolved
    int                  device_count;
    engine_scan_device_t devices[ENGINE_SCAN_MAX];
} engine_scan_state_t;

// ── Config ─────────────────────────────────────────────────────────────

typedef struct {
    bd_addr_t   target_addr;             // XM5 BD_ADDR; pass {0} if unpaired
    const char* local_name;              // GAP local name, e.g. "win-ldac"
    uint32_t    reconnect_interval_ms;   // default 5000
    engine_bitrate_mode_t initial_bitrate_mode;
} engine_config_t;

// ── Lifecycle ──────────────────────────────────────────────────────────

// One-shot init. Brings up WASAPI loopback, BTstack memory/run-loop,
// HCI transport, Realtek chipset glue, A2DP source + LDAC SEP, SDP,
// link-key DB, reconnect supervisor — everything except actually
// powering HCI on. Returns 0 on success.
int  engine_init(const engine_config_t* cfg);

// Power HCI on and enter the BTstack run loop. Blocks until
// engine_request_stop() is called from any thread. Returns when the
// run loop exits cleanly.
void engine_run(void);

// Ask the engine to shut down. Safe to call from any thread.
void engine_request_stop(void);

// Release all resources. Call after engine_run() returns.
void engine_shutdown(void);

// ── Cross-thread queries ───────────────────────────────────────────────

// Atomic-ish snapshot of current engine state. Safe from any thread.
void engine_get_status_snapshot(engine_status_t* out);

// ── Cross-thread commands ──────────────────────────────────────────────
// All of these dispatch their work onto the BT run-loop thread via
// btstack_run_loop_execute_on_main_thread; they are safe from the GUI
// thread or any other.

void engine_post_set_bitrate_mode(engine_bitrate_mode_t mode);

// Drop the current A2DP link and let the reconnect supervisor try
// again on its normal cadence. Used by the GUI "Connection Refresh"
// button. No-op if no link is up.
void engine_post_disconnect(void);

// Change the target device. The current connection (if any) is
// dropped and the reconnect supervisor is (re)started with the new
// address. Used by the pairing flow.
void engine_post_set_target(const bd_addr_t addr);

// Clear the configured target — engine stops trying to (re)connect
// and goes into the "awaiting pairing" idle state. Used when the
// user un-pairs from the GUI.
void engine_post_clear_target(void);

// ── Scan API (M8) ──────────────────────────────────────────────────────
// Read the current scan state. Safe from any thread.
void engine_get_scan_state(engine_scan_state_t* out);

// Start a GAP Classic inquiry. `duration_units` is the BTstack duration
// value: 1..30, each unit = 1.28 s. 8 ≈ 10 s is a good default. If a
// scan is already running this is a no-op.
void engine_post_start_scan(uint8_t duration_units);

// Cancel an ongoing inquiry. Pending remote-name requests still
// complete and update the snapshot. No-op if no scan is active.
void engine_post_stop_scan(void);

// Drop the previous scan's device list. Called by the GUI when the
// pair modal re-opens so stale entries don't persist.
void engine_post_clear_scan_results(void);

// ── Logging hook ───────────────────────────────────────────────────────
// Optional callback the engine fires for human-readable status events
// (one line per event, no trailing newline). Invoked on the BT thread.
// Default behaviour: write to stdout via printf.
typedef void (*engine_log_fn)(const char* line);
void engine_set_log_callback(engine_log_fn fn);

#ifdef __cplusplus
}
#endif

#endif  // WIN_LDAC_ENGINE_H
