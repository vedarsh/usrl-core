/**
 * test_usrl_e2e.c
 * End-to-end torture test for USRL unified API (pub/sub + backpressure + health + logging).
 *
 * Assumptions based on your posted usrl_api.c:
 * - usrl_init(const usrl_sys_config_t*)
 * - usrl_shutdown(usrl_ctx_t*)
 * - usrl_pub_create(usrl_ctx_t*, const usrl_pub_config_t*)
 * - usrl_pub_send(usrl_pub_t*, const void*, uint32_t)
 * - usrl_pub_get_health(usrl_pub_t*, usrl_health_t*)
 * - usrl_pub_destroy(usrl_pub_t*)
 * - usrl_sub_create(usrl_ctx_t*, const char*)
 * - usrl_sub_recv(usrl_sub_t*, void*, uint32_t) returns -11 for EAGAIN
 * - usrl_sub_get_health(usrl_sub_t*, usrl_health_t*)
 * - usrl_sub_destroy(usrl_sub_t*)
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <stdatomic.h>

#include "usrl.h"

/* ---------------------------- Small test framework ---------------------------- */

static int g_fail = 0;

#define TLOG(fmt, ...)  do { fprintf(stdout, fmt "\n", ##__VA_ARGS__); fflush(stdout); } while (0)
#define TERR(fmt, ...)  do { fprintf(stderr, "[ERR] " fmt "\n", ##__VA_ARGS__); fflush(stderr); } while (0)

#define CHECK(cond, fmt, ...) \
    do { if (!(cond)) { g_fail = 1; TERR("FAIL: " fmt, ##__VA_ARGS__); } } while (0)

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void msleep(uint32_t ms) { usleep((useconds_t)ms * 1000u); }

/* ---------------------------- Shared counters ---------------------------- */

typedef struct {
    atomic_uint_fast64_t pub_ok;
    atomic_uint_fast64_t pub_err;
    atomic_uint_fast64_t sub_ok;
    atomic_uint_fast64_t sub_err;
} counters_t;

/* ---------------------------- Thread args ---------------------------- */

typedef struct {
    usrl_ctx_t *ctx;
    const char *topic;
    uint32_t slot_count;
    uint32_t slot_size;
    int ring_type;           /* USRL_RING_SWMR / USRL_RING_MWMR */
    bool block_on_full;
    uint32_t rate_limit_hz;  /* 0 disables limiter */
    uint32_t msgs;
    uint32_t payload_len;
    uint32_t pause_every;    /* 0 disables */
    uint32_t pause_us;       /* pause duration */
    counters_t *ctr;
} pub_args_t;

typedef struct {
    usrl_ctx_t *ctx;
    const char *topic;
    uint32_t max_len;
    uint32_t run_ms;
    uint32_t poll_sleep_us;  /* sleep on -11 */
    counters_t *ctr;
} sub_args_t;

/* ---------------------------- Publisher thread ---------------------------- */

static void* pub_main(void *arg) {
    pub_args_t *pa = (pub_args_t*)arg;

    usrl_pub_config_t pcfg;
    memset(&pcfg, 0, sizeof(pcfg));
    pcfg.topic = pa->topic;
    pcfg.slot_count = pa->slot_count;
    pcfg.slot_size = pa->slot_size;
    pcfg.block_on_full = pa->block_on_full;
    pcfg.rate_limit_hz = (int)pa->rate_limit_hz;
    pcfg.ring_type = pa->ring_type;

    usrl_pub_t *pub = usrl_pub_create(pa->ctx, &pcfg);
    if (!pub) {
        atomic_fetch_add(&pa->ctr->pub_err, 1);
        TERR("[PUB %s] create failed", pa->topic);
        return NULL;
    }

    uint8_t *buf = (uint8_t*)malloc(pa->payload_len);
    if (!buf) {
        atomic_fetch_add(&pa->ctr->pub_err, 1);
        TERR("[PUB %s] malloc failed", pa->topic);
        usrl_pub_destroy(pub);
        return NULL;
    }
    memset(buf, 0xAB, pa->payload_len);

    for (uint32_t i = 0; i < pa->msgs; i++) {
        /* stamp a sequence for debugging */
        if (pa->payload_len >= 8) {
            uint64_t seq = (uint64_t)i + 1;
            memcpy(buf, &seq, sizeof(seq));
        }

        int rc = usrl_pub_send(pub, buf, pa->payload_len);
        if (rc == 0) atomic_fetch_add(&pa->ctr->pub_ok, 1);
        else         atomic_fetch_add(&pa->ctr->pub_err, 1);

        if (pa->pause_every && (i % pa->pause_every == 0) && pa->pause_us) {
            usleep(pa->pause_us);
        }
    }

    usrl_health_t h;
    memset(&h, 0, sizeof(h));
    usrl_pub_get_health(pub, &h);

    TLOG("[PUB %s] health: ops=%llu errors=%llu lag=%llu healthy=%d rate_hz=%.2f",
         pa->topic,
         (unsigned long long)h.operations,
         (unsigned long long)h.errors,
         (unsigned long long)h.lag,
         (int)h.healthy,
         h.rate_hz);

    free(buf);
    usrl_pub_destroy(pub);
    return NULL;
}

/* ---------------------------- Subscriber thread ---------------------------- */

static void* sub_main(void *arg) {
    sub_args_t *sa = (sub_args_t*)arg;

    usrl_sub_t *sub = usrl_sub_create(sa->ctx, sa->topic);
    if (!sub) {
        atomic_fetch_add(&sa->ctr->sub_err, 1);
        TERR("[SUB %s] create failed", sa->topic);
        return NULL;
    }

    uint8_t *buf = (uint8_t*)malloc(sa->max_len);
    if (!buf) {
        atomic_fetch_add(&sa->ctr->sub_err, 1);
        TERR("[SUB %s] malloc failed", sa->topic);
        usrl_sub_destroy(sub);
        return NULL;
    }

    uint64_t deadline = now_ns() + (uint64_t)sa->run_ms * 1000000ull;
    while (now_ns() < deadline) {
        int n = usrl_sub_recv(sub, buf, sa->max_len);
        if (n > 0) {
            atomic_fetch_add(&sa->ctr->sub_ok, 1);
        } else if (n == -11) {
            if (sa->poll_sleep_us) usleep(sa->poll_sleep_us);
        } else {
            atomic_fetch_add(&sa->ctr->sub_err, 1);
            /* small pause to avoid tight error loops */
            usleep(100);
        }
    }

    usrl_health_t h;
    memset(&h, 0, sizeof(h));
    usrl_sub_get_health(sub, &h);

    TLOG("[SUB %s] health: ops=%llu errors=%llu lag=%llu healthy=%d rate_hz=%.2f",
         sa->topic,
         (unsigned long long)h.operations,
         (unsigned long long)h.errors,
         (unsigned long long)h.lag,
         (int)h.healthy,
         h.rate_hz);

    free(buf);
    usrl_sub_destroy(sub);
    return NULL;
}

/* ---------------------------- Phases ---------------------------- */

static int phase_rate_limit_drop(usrl_ctx_t *ctx) {
    TLOG("========================================================");
    TLOG("[PHASE] Backpressure (rate limit) + non-blocking drops");
    TLOG("========================================================");

    counters_t ctr;
    memset(&ctr, 0, sizeof(ctr));

    pthread_t tp, ts;

    /* Subscriber runs while publisher hammers; buffer large enough to not truncate */
    sub_args_t sa = {
        .ctx = ctx,
        .topic = "bp_swmr",
        .max_len = 256,
        .run_ms = 1200,
        .poll_sleep_us = 200,
        .ctr = &ctr
    };

    pub_args_t pa = {
        .ctx = ctx,
        .topic = "bp_swmr",
        .slot_count = 64,
        .slot_size = 256,
        .ring_type = USRL_RING_SWMR,
        .block_on_full = false,
        .rate_limit_hz = 50,      /* intentionally low */
        .msgs = 5000,
        .payload_len = 64,
        .pause_every = 0,
        .pause_us = 0,
        .ctr = &ctr
    };

    pthread_create(&ts, NULL, sub_main, &sa);
    usleep(10000);
    pthread_create(&tp, NULL, pub_main, &pa);

    pthread_join(tp, NULL);
    pthread_join(ts, NULL);

    uint64_t ok  = atomic_load(&ctr.pub_ok);
    uint64_t err = atomic_load(&ctr.pub_err);

    TLOG("[PHASE] pub_ok=%llu pub_err=%llu sub_ok=%llu sub_err=%llu",
         (unsigned long long)ok,
         (unsigned long long)err,
         (unsigned long long)atomic_load(&ctr.sub_ok),
         (unsigned long long)atomic_load(&ctr.sub_err));

    CHECK(err > 0, "Expected drops/errors due to rate limiter, got pub_err=0");
    CHECK(ok  > 0, "Expected at least some publishes to pass, got pub_ok=0");
    return g_fail ? -1 : 0;
}

static int phase_overwrite_lag(usrl_ctx_t *ctx) {
    TLOG("========================================================");
    TLOG("[PHASE] Subscriber lag + ring overwrite (small ring)");
    TLOG("========================================================");

    counters_t ctr;
    memset(&ctr, 0, sizeof(ctr));

    pthread_t tp, ts;

    /* Start publisher first, subscriber starts later to force lag */
    pub_args_t pa = {
        .ctx = ctx,
        .topic = "ow_swmr",
        .slot_count = 16,     /* tiny ring => overwrite quickly */
        .slot_size = 256,
        .ring_type = USRL_RING_SWMR,
        .block_on_full = false, /* allow overwrite pressure behavior */
        .rate_limit_hz = 0,
        .msgs = 4000,
        .payload_len = 64,
        .pause_every = 0,
        .pause_us = 0,
        .ctr = &ctr
    };

    sub_args_t sa = {
        .ctx = ctx,
        .topic = "ow_swmr",
        .max_len = 256,
        .run_ms = 1200,
        .poll_sleep_us = 200,
        .ctr = &ctr
    };

    pthread_create(&tp, NULL, pub_main, &pa);
    /* Force subscriber to start late */
    msleep(200);
    pthread_create(&ts, NULL, sub_main, &sa);

    pthread_join(tp, NULL);
    pthread_join(ts, NULL);

    /* We can only assert "some data moved" from public surface */
    CHECK(atomic_load(&ctr.sub_ok) > 0, "Expected subscriber to receive some messages");
    return g_fail ? -1 : 0;
}

static int phase_truncation(usrl_ctx_t *ctx) {
    TLOG("========================================================");
    TLOG("[PHASE] Truncation (subscriber buffer too small)");
    TLOG("========================================================");

    counters_t ctr;
    memset(&ctr, 0, sizeof(ctr));

    pthread_t tp, ts;

    /* Subscriber max_len smaller than payload_len => expect sub_err increments */
    sub_args_t sa = {
        .ctx = ctx,
        .topic = "trunc_swmr",
        .max_len = 64,       /* smaller than payload */
        .run_ms = 900,
        .poll_sleep_us = 200,
        .ctr = &ctr
    };

    pub_args_t pa = {
        .ctx = ctx,
        .topic = "trunc_swmr",
        .slot_count = 64,
        .slot_size = 256,     /* slots can hold the payload */
        .ring_type = USRL_RING_SWMR,
        .block_on_full = false,
        .rate_limit_hz = 0,
        .msgs = 1000,
        .payload_len = 200,   /* > 64 => truncation on sub */
        .pause_every = 0,
        .pause_us = 0,
        .ctr = &ctr
    };

    pthread_create(&ts, NULL, sub_main, &sa);
    usleep(10000);
    pthread_create(&tp, NULL, pub_main, &pa);

    pthread_join(tp, NULL);
    pthread_join(ts, NULL);

    CHECK(atomic_load(&ctr.sub_err) > 0, "Expected truncation errors, got sub_err=0");
    return g_fail ? -1 : 0;
}

static int phase_mwmr(usrl_ctx_t *ctx) {
    TLOG("========================================================");
    TLOG("[PHASE] MWMR contention (2 publishers, 1 subscriber)");
    TLOG("========================================================");

    counters_t ctr;
    memset(&ctr, 0, sizeof(ctr));

    pthread_t tp1, tp2, ts;

    sub_args_t sa = {
        .ctx = ctx,
        .topic = "mw_bus",
        .max_len = 256,
        .run_ms = 1200,
        .poll_sleep_us = 100,
        .ctr = &ctr
    };

    pub_args_t pa1 = {
        .ctx = ctx,
        .topic = "mw_bus",
        .slot_count = 256,
        .slot_size = 256,
        .ring_type = USRL_RING_MWMR,
        .block_on_full = false,
        .rate_limit_hz = 0,
        .msgs = 5000,
        .payload_len = 64,
        .pause_every = 0,
        .pause_us = 0,
        .ctr = &ctr
    };

    pub_args_t pa2 = pa1;

    pthread_create(&ts, NULL, sub_main, &sa);
    usleep(10000);
    pthread_create(&tp1, NULL, pub_main, &pa1);
    pthread_create(&tp2, NULL, pub_main, &pa2);

    pthread_join(tp1, NULL);
    pthread_join(tp2, NULL);
    pthread_join(ts, NULL);

    CHECK(atomic_load(&ctr.sub_ok) > 0, "Expected subscriber to receive messages in MWMR test");
    return g_fail ? -1 : 0;
}

/* ---------------------------- Main ---------------------------- */

int main(void) {
    usrl_sys_config_t sys;
    memset(&sys, 0, sizeof(sys));
    sys.app_name = "usrl_e2e";
    sys.log_file_path = NULL;     /* stdout logging if supported */
    sys.log_level = USRL_LOG_INFO;

    usrl_ctx_t *ctx = usrl_init(&sys);
    if (!ctx) {
        TERR("usrl_init failed");
        return 2;
    }

    (void)phase_rate_limit_drop(ctx);
    (void)phase_overwrite_lag(ctx);
    (void)phase_truncation(ctx);
    (void)phase_mwmr(ctx);

    usrl_shutdown(ctx);

    if (g_fail) {
        TLOG("========================================================");
        TLOG("RESULT: FAIL");
        TLOG("========================================================");
        return 1;
    }

    TLOG("========================================================");
    TLOG("RESULT: PASS");
    TLOG("========================================================");
    return 0;
}
