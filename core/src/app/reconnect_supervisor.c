// reconnect_supervisor — see reconnect_supervisor.h.

#include "app/reconnect_supervisor.h"

#include <stdio.h>
#include <string.h>

#include "btstack_run_loop.h"
#include "btstack_util.h"

typedef enum {
    RS_IDLE,
    RS_RUNNING,    // either currently connected, or waiting on the retry timer
    RS_STOPPED,
} rs_state_t;

static struct {
    bd_addr_t                       target;
    reconnect_supervisor_attempt_fn attempt_fn;
    uint32_t                        retry_interval_ms;
    btstack_timer_source_t          timer;
    rs_state_t                      state;
    int                             attempt_count;
    bool                            timer_armed;
} s;

static void cancel_timer(void) {
    if (s.timer_armed) {
        btstack_run_loop_remove_timer(&s.timer);
        s.timer_armed = false;
    }
}

static void fire_attempt(btstack_timer_source_t* ts) {
    (void)ts;
    s.timer_armed = false;
    if (s.state == RS_STOPPED) return;
    s.attempt_count++;
    printf("[reconnect] attempt #%d → %s\n",
           s.attempt_count, bd_addr_to_str(s.target));
    s.attempt_fn();
    // Result arrives later via note_connected / note_disconnected. If
    // BTstack neither connects nor explicitly fails (page timeout doesn't
    // surface as an A2DP event in older builds), the caller should still
    // see a HCI_EVENT_CONNECTION_COMPLETE failure or A2DP signaling
    // connection-established with non-SUCCESS status. Either of those
    // funnel into note_disconnected, which arms a fresh timer.
}

static void schedule_next(void) {
    cancel_timer();
    if (s.state == RS_STOPPED) return;
    btstack_run_loop_set_timer_handler(&s.timer, &fire_attempt);
    btstack_run_loop_set_timer(&s.timer, s.retry_interval_ms);
    btstack_run_loop_add_timer(&s.timer);
    s.timer_armed = true;
}

void reconnect_supervisor_init(const bd_addr_t target,
                               reconnect_supervisor_attempt_fn attempt_fn,
                               uint32_t retry_interval_ms) {
    memset(&s, 0, sizeof(s));
    memcpy(s.target, target, sizeof(bd_addr_t));
    s.attempt_fn        = attempt_fn;
    s.retry_interval_ms = retry_interval_ms ? retry_interval_ms : 5000;
    s.state             = RS_IDLE;
}

void reconnect_supervisor_start(void) {
    if (s.state != RS_IDLE) return;
    s.state = RS_RUNNING;
    // First attempt is immediate; no point waiting `retry_interval_ms`
    // before even trying once.
    s.attempt_count++;
    printf("[reconnect] attempt #%d → %s (initial)\n",
           s.attempt_count, bd_addr_to_str(s.target));
    s.attempt_fn();
}

void reconnect_supervisor_note_connected(void) {
    if (s.state == RS_STOPPED) return;
    cancel_timer();
    if (s.attempt_count > 1) {
        printf("[reconnect] connected after %d attempt%s.\n",
               s.attempt_count, s.attempt_count == 1 ? "" : "s");
    }
    s.attempt_count = 0;
    // Stay in RS_RUNNING so future note_disconnected fires a new timer.
    s.state = RS_RUNNING;
}

void reconnect_supervisor_note_disconnected(void) {
    if (s.state == RS_STOPPED) return;
    s.state = RS_RUNNING;
    printf("[reconnect] disconnected. Retry in %u ms ...\n",
           s.retry_interval_ms);
    schedule_next();
}

void reconnect_supervisor_stop(void) {
    s.state = RS_STOPPED;
    cancel_timer();
}

int reconnect_supervisor_attempt_count(void) {
    return s.attempt_count;
}
