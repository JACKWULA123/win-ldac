// reconnect_supervisor — keep one A2DP link "always trying to be up".
//
// Models the behaviour of a normal Bluetooth headphone pair: while the
// app is running, we keep retrying the connection to the target sink
// until it comes up, and we restart that loop the moment it drops.
// Existing pairing/link key is reused (BTstack handles that via the
// link-key DB installed by M6.1) — the user never sees a re-pair prompt.
//
// Usage:
//   1. reconnect_supervisor_init(bd_addr, &attempt_fn, retry_interval_ms);
//   2. reconnect_supervisor_start();   // fires the first attempt
//   3. Forward link state changes:
//        on signalling-connection-established (status=SUCCESS) →
//            reconnect_supervisor_note_connected()
//        on signalling-connection-established (status!=SUCCESS) or
//        on signalling-connection-released                     →
//            reconnect_supervisor_note_disconnected()
//   4. reconnect_supervisor_stop() — terminates the loop (no more retries).
//
// `attempt_fn` is invoked from the BTstack run loop thread; it should
// drop any stream-specific state (teardown encoder, clear cids) and
// call `a2dp_source_establish_stream`. The result will arrive
// asynchronously via BTstack events, which the caller funnels back into
// note_connected / note_disconnected.

#ifndef WIN_LDAC_RECONNECT_SUPERVISOR_H
#define WIN_LDAC_RECONNECT_SUPERVISOR_H

#include <stdint.h>

#include "bluetooth.h"  // bd_addr_t

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*reconnect_supervisor_attempt_fn)(void);

// One-shot init. `retry_interval_ms` is the delay between successive
// reconnect attempts while disconnected. 5000 is a reasonable default
// for the XM5 — the BT page-timeout already eats ~3 s, so anything
// shorter just thrashes the dongle.
void reconnect_supervisor_init(const bd_addr_t target,
                               reconnect_supervisor_attempt_fn attempt_fn,
                               uint32_t retry_interval_ms);

// Kick off the first attempt now. After this, the supervisor stays
// "running" until reconnect_supervisor_stop() is called.
void reconnect_supervisor_start(void);

// Tell the supervisor a connect succeeded. Cancels any pending retry
// timer; resets the attempt counter.
void reconnect_supervisor_note_connected(void);

// Tell the supervisor the link is down (or a connect attempt failed).
// Schedules another attempt in `retry_interval_ms`. Repeatable: calling
// this when a timer is already pending just resets it.
void reconnect_supervisor_note_disconnected(void);

// Stop the supervisor permanently (user shutdown). Idempotent.
void reconnect_supervisor_stop(void);

// Diagnostics.
int  reconnect_supervisor_attempt_count(void);

#ifdef __cplusplus
}
#endif

#endif  // WIN_LDAC_RECONNECT_SUPERVISOR_H
