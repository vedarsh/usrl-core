/**
 * @file usrl_api.c
 * @brief Unified Facade Implementation (Production Ready).
 */

#define _GNU_SOURCE
#include "usrl.h"
#include "usrl_core.h"
#include "usrl_ring.h"
#include "usrl_backpressure.h"
#include "usrl_health.h"
#include "usrl_logging.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/mman.h>

/* For shm size discovery (publisher/subscriber) */
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>

#ifndef USRL_MIN
#define USRL_MIN(a,b) ((a) < (b) ? (a) : (b))
#endif

uint64_t usrl_swmr_total_published(void *ring_desc);

static uint32_t g_pub_id_seq = 1;

/* ============================================================================
 * DEFAULT SHM SIZING (SETTER)
 * ============================================================================ */

static uint32_t g_default_shm_size_mb = 64;

void usrl_set_default_shm_size_mb(uint32_t mb)
{
    if (mb < 8) mb = 8;
    g_default_shm_size_mb = mb;
}

static size_t usrl__choose_shm_size(size_t ring_size)
{
    size_t min_default = (size_t)g_default_shm_size_mb * 1024u * 1024u;
    return (ring_size > min_default) ? ring_size : min_default;
}

/* usrl_backoff_exponential returns ns; usleep expects us */
static inline useconds_t usrl__ns_to_us_ceil(uint64_t ns)
{
    if (ns == 0) return 0;
    uint64_t us = (ns + 999ull) / 1000ull;
    if (us == 0) us = 1;
    if (us > 10000000ull) us = 10000000ull;
    return (useconds_t)us;
}

/* ============================================================================
 * SHM size helper (shared)
 * ============================================================================ */

static size_t usrl__shm_object_size_bytes(const char *shm_path)
{
    int fd = shm_open(shm_path, O_RDONLY, 0);
    if (fd < 0) return 0;

    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        return 0;
    }
    close(fd);

    if (st.st_size <= 0) return 0;
    return (size_t)st.st_size;
}

/* ============================================================================
 * INTERNAL STRUCTURES
 * ============================================================================ */

struct usrl_ctx { char name[64]; };

struct usrl_pub {
    usrl_ctx_t *ctx;
    UsrlPublisher core;
    UsrlMwmrPublisher core_mw;
    PublishQuota quota;
    bool block_on_full;
    bool use_limiter;
    bool is_mwmr;
    char topic[64];
    void *shm_base;
    size_t map_size;
    uint64_t local_drops;
};

struct usrl_sub {
    usrl_ctx_t *ctx;
    UsrlSubscriber core;
    char topic[64];
    void *shm_base;
    size_t map_size;
    uint64_t local_ops;
    uint64_t local_skips;
    uint64_t local_errors;
};

/* ============================================================================
 * LIFECYCLE
 * ============================================================================ */

usrl_ctx_t *usrl_init(const usrl_sys_config_t *config)
{
    if (!config) return NULL;
    usrl_logging_init(config->log_file_path, config->log_level);

    usrl_ctx_t *ctx = calloc(1, sizeof(usrl_ctx_t));
    if (!ctx) return NULL;

    if (config->app_name) strncpy(ctx->name, config->app_name, 63);
    else strcpy(ctx->name, "usrl_app");
    ctx->name[63] = '\0';

    USRL_INFO("API", "USRL System Initialized: %s", ctx->name);
    return ctx;
}

void usrl_shutdown(usrl_ctx_t *ctx)
{
    if (!ctx) return;
    USRL_INFO("API", "USRL System Shutdown: %s", ctx->name);
    usrl_logging_shutdown();
    free(ctx);
}

/* ============================================================================
 * PUBLISHER
 * ============================================================================ */

usrl_pub_t *usrl_pub_create(usrl_ctx_t *ctx, const usrl_pub_config_t *config)
{
    if (!ctx || !config || !config->topic) return NULL;

    uint32_t sc = (config->slot_count > 0) ? config->slot_count : 4096;
    uint32_t ss = (config->slot_size  > 0) ? config->slot_size  : 1024;

    size_t ring_size = (size_t)sc * (size_t)ss + (1024u * 1024u);
    size_t requested_shm_size = usrl__choose_shm_size(ring_size);

    char shm_path[128];
    snprintf(shm_path, sizeof(shm_path), "/usrl-%s", config->topic);

    UsrlTopicConfig tcfg;
    memset(&tcfg, 0, sizeof(tcfg));
    strncpy(tcfg.name, config->topic, 63);
    tcfg.name[63] = '\0';
    tcfg.slot_count = sc;
    tcfg.slot_size  = ss;
    tcfg.type = (config->ring_type == USRL_RING_MWMR) ? USRL_RING_TYPE_MWMR : USRL_RING_TYPE_SWMR;

    int irc = usrl_core_init(shm_path, requested_shm_size, &tcfg, 1);
    if (irc < 0) {
        USRL_ERROR("API", "Core init failed topic=%s rc=%d errno=%d", config->topic, irc, errno);
        return NULL;
    } else if (irc == 1) {
        /* Already exists: normal in MWMR / multi-pub attach. */
        USRL_DEBUG("API", "Core exists topic=%s; attaching", config->topic);
    }

    /* Map using actual SHM object size (avoid mismatched munmap sizes later). */
    size_t obj_size = usrl__shm_object_size_bytes(shm_path);
    if (obj_size == 0) {
        USRL_ERROR("API", "Publisher cannot open/fstat topic=%s path=%s errno=%d",
                   config->topic, shm_path, errno);
        return NULL;
    }

    void *base = usrl_core_map(shm_path, obj_size);
    if (!base) {
        USRL_ERROR("API", "Publisher mmap failed topic=%s size=%zu errno=%d",
                   config->topic, obj_size, errno);
        return NULL;
    }

    usrl_pub_t *pub = calloc(1, sizeof(usrl_pub_t));
    if (!pub) {
        munmap(base, obj_size);
        return NULL;
    }

    pub->ctx = ctx;
    pub->shm_base = base;
    pub->map_size = obj_size;
    pub->block_on_full = config->block_on_full;
    pub->is_mwmr = (config->ring_type == USRL_RING_MWMR);
    strncpy(pub->topic, config->topic, 63);
    pub->topic[63] = '\0';

    if (config->rate_limit_hz > 0) {
        usrl_quota_init(&pub->quota, (uint64_t)config->rate_limit_hz);
        pub->use_limiter = true;
    }

    uint32_t my_id = __sync_fetch_and_add(&g_pub_id_seq, 1);

    if (pub->is_mwmr) usrl_mwmr_pub_init(&pub->core_mw, base, config->topic, my_id);
    else             usrl_pub_init(&pub->core,    base, config->topic, my_id);

    return pub;
}

int usrl_pub_send(usrl_pub_t *pub, const void *data, uint32_t len)
{
    if (!pub || !data) return -1;

    /* usrl_quota_check(): 1 = THROTTLED, 0 = allowed */
    if (pub->use_limiter) {
        if (usrl_quota_check(&pub->quota)) {
            if (pub->block_on_full) {
                useconds_t us = usrl__ns_to_us_ceil(usrl_backoff_exponential(1));
                if (us) usleep(us);
            } else {
                pub->local_drops++;
                return -1;
            }
        }
    }

    int res;
    if (pub->is_mwmr) {
        res = usrl_mwmr_pub_publish(&pub->core_mw, data, len);
        while ((res == USRL_RING_FULL || res == USRL_RING_TIMEOUT) && pub->block_on_full) {
            usleep(1);
            res = usrl_mwmr_pub_publish(&pub->core_mw, data, len);
        }
    } else {
        res = usrl_pub_publish(&pub->core, data, len);
        while (res == USRL_RING_FULL && pub->block_on_full) {
            usleep(1);
            res = usrl_pub_publish(&pub->core, data, len);
        }
    }

    if (res == USRL_RING_OK) return 0;

    if (res == USRL_RING_FULL) pub->local_drops++;
    return -1;
}

void usrl_pub_get_health(usrl_pub_t *pub, usrl_health_t *out)
{
    if (!pub || !out) return;

    RingHealth *rh = usrl_health_get(pub->shm_base, pub->topic);
    if (rh) {
        out->operations = rh->pub_health.total_published;
        out->rate_hz    = rh->pub_health.publish_rate_hz;
        out->errors     = pub->local_drops;
        out->lag        = 0;
        out->healthy    = (out->errors == 0);
        usrl_health_free(rh);
    } else {
        memset(out, 0, sizeof(*out));
        out->errors = pub->local_drops;
    }
}

void usrl_pub_destroy(usrl_pub_t *pub)
{
    if (!pub) return;
    if (pub->shm_base && pub->map_size > 0) munmap(pub->shm_base, pub->map_size);
    free(pub);
}

/* ============================================================================
 * SUBSCRIBER
 * ============================================================================ */

usrl_sub_t *usrl_sub_create(usrl_ctx_t *ctx, const char *topic)
{
    if (!ctx || !topic) return NULL;

    char shm_path[128];
    snprintf(shm_path, sizeof(shm_path), "/usrl-%s", topic);

    size_t map_size = usrl__shm_object_size_bytes(shm_path);
    if (map_size == 0) {
        USRL_ERROR("API", "Subscriber cannot open/fstat topic='%s' (path=%s) errno=%d",
                   topic, shm_path, errno);
        return NULL;
    }

    void *base = usrl_core_map(shm_path, map_size);
    if (!base) {
        USRL_ERROR("API", "Subscriber mmap failed topic='%s' size=%zu errno=%d",
                   topic, map_size, errno);
        return NULL;
    }

    usrl_sub_t *sub = calloc(1, sizeof(usrl_sub_t));
    if (!sub) {
        munmap(base, map_size);
        return NULL;
    }

    sub->ctx = ctx;
    sub->shm_base = base;
    sub->map_size = map_size;
    strncpy(sub->topic, topic, 63);
    sub->topic[63] = '\0';

    usrl_sub_init(&sub->core, base, topic);
    return sub;
}

int usrl_sub_recv(usrl_sub_t *sub, void *buffer, uint32_t max_len)
{
    if (!sub || !buffer) return -1;

    int ret = usrl_sub_next(&sub->core, buffer, max_len, NULL);

    if (ret == USRL_RING_NO_DATA) return -11;

    if (ret == USRL_RING_TRUNC) {
        sub->local_skips++;
        return -1;
    }

    if (ret == USRL_RING_ERROR) {
        sub->local_errors++;
        return -1;
    }

    sub->local_ops++;
    return ret;
}

void usrl_sub_get_health(usrl_sub_t *sub, usrl_health_t *out)
{
    if (!sub || !out) return;

    out->operations = sub->local_ops;
    out->errors     = sub->local_skips + sub->local_errors + sub->core.skipped_count;
    out->rate_hz    = 0;

    if (sub->core.desc) {
        uint64_t w_head = usrl_swmr_total_published(sub->core.desc);
        uint64_t my_seq = sub->core.last_seq;
        out->lag = (w_head > my_seq) ? (w_head - my_seq) : 0;
    } else {
        out->lag = 0;
    }

    out->healthy = (out->lag < 100 && out->errors == 0);
}

void usrl_sub_destroy(usrl_sub_t *sub)
{
    if (!sub) return;
    if (sub->shm_base && sub->map_size > 0) munmap(sub->shm_base, sub->map_size);
    free(sub);
}
