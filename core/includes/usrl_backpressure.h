#ifndef USRL_BACKPRESSURE_H
#define USRL_BACKPRESSURE_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint64_t publish_quota;          /* messages allowed per window */
    uint64_t publish_window_ns;      /* window length in ns */
    uint64_t last_window_start_ns;   /* start time of current window */
    uint64_t msgs_in_window;         /* count in current window */
    uint64_t total_throttled;        /* total throttled events */
} PublishQuota;

typedef enum {
    USRL_BP_NONE = 0,
    USRL_BP_DROP,
    USRL_BP_BLOCK,
    USRL_BP_THROTTLE,
} UsrlBackpressureMode;

typedef struct {
    uint64_t subscriber_pos;
    uint64_t writer_pos;
    uint64_t lag_slots;
    uint64_t lag_threshold;
    bool is_lagging;
} UsrlLagTracker;

/*
 * Fixed-window rate limiter init.
 *
 * Design:
 * - 1ms windows (1,000,000 ns) to keep counters bounded and checks cheap.
 * - publish_quota is msgs allowed per 1ms window.
 * - We round up so low rates (e.g., 50 msg/sec) still allow traffic:
 *     quota_per_ms = ceil(msgs_per_sec / 1000).
 */
static inline void usrl_quota_init(PublishQuota *quota, uint64_t msgs_per_sec)
{
    if (!quota) return;

    /* Always initialize */
    quota->publish_window_ns = 1000000ULL; /* 1 ms */
    quota->last_window_start_ns = 0;
    quota->msgs_in_window = 0;
    quota->total_throttled = 0;

    if (msgs_per_sec == 0) {
        /* Disabled limiter: allow "effectively infinite" per window */
        quota->publish_quota = UINT64_MAX;
        return;
    }

    uint64_t per_ms = (msgs_per_sec + 999ULL) / 1000ULL; /* ceil division */
    if (per_ms == 0) per_ms = 1;
    quota->publish_quota = per_ms;
}

int usrl_quota_check(PublishQuota *quota); /* returns 1 when throttled, 0 when allowed */
int usrl_backpressure_check_lag(uint64_t lag, uint64_t threshold);
uint64_t usrl_backoff_exponential(uint32_t attempt); /* ns */
uint64_t usrl_backoff_linear(uint64_t lag, uint64_t max_lag); /* us (as currently implemented) */

#endif /* USRL_BACKPRESSURE_H */
