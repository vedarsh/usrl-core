#include "usrl_core.h"
#include "usrl_ring.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

/* --------------------------------------------------------------------------
 * Debug Utilities
 * -------------------------------------------------------------------------- */
#ifdef DEBUG
#define DEBUG_PRINT_RING(...) \
    do { printf("[DEBUG][RING] " __VA_ARGS__); fflush(stdout); } while (0)
#else
#define DEBUG_PRINT_RING(...) ((void)0)
#endif

/* --------------------------------------------------------------------------
 * FIX #6: Use CLOCK_MONOTONIC instead of CLOCK_REALTIME
 *
 * CLOCK_REALTIME can jump backwards (NTP, admin changes, VM clock skew).
 * CLOCK_MONOTONIC is guaranteed monotonic and suitable for timing.
 * -------------------------------------------------------------------------- */
static inline uint64_t usrl_timestamp_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/* =============================================================================
 * PUBLISHER INITIALIZATION
 * =============================================================================
 *
 * usrl_pub_init()
 *
 * Sets up a publisher handle for a specific topic. The publisher stores:
 *
 *   - RingDesc ptr   (metadata for the ring)
 *   - Base pointer   (beginning of slots)
 *   - Mask           (slot_count - 1; enables fast index wrap)
 *   - pub_id         (publisher identity)
 *
 * No shared memory is allocated here. Everything is already laid out by
 * usrl_core_init(). We simply bind the publisher to an existing topic.
 *
 * =============================================================================
 */
void usrl_pub_init(
    UsrlPublisher *p,
    void          *core_base,
    const char    *topic,
    uint16_t       pub_id
) {
    if (!p || !core_base || !topic)
        return;

    /* Look up topic in core header table */
    TopicEntry *t = usrl_get_topic(core_base, topic);
    if (!t) {
        DEBUG_PRINT_RING("could not find topic\n");
        return;
    }

    /* Bind publisher to ring */
    p->desc     = (RingDesc*)((uint8_t*)core_base + t->ring_desc_offset);
    p->base_ptr = (uint8_t*)core_base + p->desc->base_offset;
    p->mask     = p->desc->slot_count - 1;
    p->pub_id   = pub_id;

    DEBUG_PRINT_RING("publisher %u ready\n", pub_id);
}

/* =============================================================================
 * PUBLISH
 * =============================================================================
 *
 * usrl_pub_publish()
 *
 * Single-producer publish path:
 *
 *   1. Reserve a slot index via atomic_fetch_add (lock-free).
 *   2. Compute slot pointer.
 *   3. Write payload, metadata, timestamp.
 *   4. Insert memory barrier (FIX #2).
 *   5. Commit by writing hdr->seq with memory_order_release.
 *
 * Consumers will not read the slot until seq == expected_commit_sequence.
 *
 * =============================================================================
 */
int usrl_pub_publish(
    UsrlPublisher *p,
    const void    *data,
    uint32_t       len
) {
    if (USRL_UNLIKELY(!p || !p->desc || !data))
        return -1;

    RingDesc *d = p->desc;

    /* FIX #7: Ensure payload fits in the configured slot */
    if (USRL_UNLIKELY(len > (d->slot_size - sizeof(SlotHeader))))
        return -2;

    /* ----------------------------------------------------------------------
     * 1. Atomic reservation
     *
     * fetch_add() gives each writer a unique slot index. This is our
     * commit sequence number.
     * ---------------------------------------------------------------------- */
    uint64_t old_head =
        atomic_fetch_add_explicit(&d->w_head, 1, memory_order_acq_rel);
    uint64_t commit_seq = old_head + 1;

    /* Ring index via mask (branchless wrap) */
    uint32_t idx   = (uint32_t)((commit_seq - 1) & p->mask);
    uint8_t *slot  = p->base_ptr + ((uint64_t)idx * d->slot_size);
    SlotHeader *hdr = (SlotHeader*)slot;

    /* FIX #11: Prefetch the slot for writing */
    USRL_PREFETCH_W(slot + sizeof(SlotHeader));

    /* ----------------------------------------------------------------------
     * 2. Write payload
     * ---------------------------------------------------------------------- */
    memcpy(slot + sizeof(SlotHeader), data, len);

    hdr->payload_len  = len;
    hdr->pub_id       = p->pub_id;

    /* FIX #6: Use monotonic timestamp */
    hdr->timestamp_ns = usrl_timestamp_ns();

    /* ----------------------------------------------------------------------
     * 3. FIX #2: Memory barrier before commit
     *
     * Ensures all payload/header writes are globally visible BEFORE the
     * sequence number is published. Without this, a reader could see the
     * new seq but stale/garbage payload data.
     * ---------------------------------------------------------------------- */
    atomic_thread_fence(memory_order_release);

    /* ----------------------------------------------------------------------
     * 4. Commit: seq is published last
     * ---------------------------------------------------------------------- */
    atomic_store_explicit(&hdr->seq, commit_seq, memory_order_release);

    DEBUG_PRINT_RING("pub %u wrote seq %lu\n", p->pub_id, commit_seq);
    return 0;
}

/* =============================================================================
 * SUBSCRIBER INITIALIZATION
 * =============================================================================
 *
 * usrl_sub_init()
 *
 * Sets up a subscriber for a topic.
 * Tracks:
 *   - RingDesc
 *   - Base pointer
 *   - Mask
 *   - last_seq (tracks which sequence number to read next)
 *
 * =============================================================================
 */
void usrl_sub_init(
    UsrlSubscriber *s,
    void           *core_base,
    const char     *topic
) {
    if (!s || !core_base || !topic)
        return;

    TopicEntry *t = usrl_get_topic(core_base, topic);
    if (!t)
        return;

    s->desc     = (RingDesc*)((uint8_t*)core_base + t->ring_desc_offset);
    s->base_ptr = (uint8_t*)core_base + s->desc->base_offset;
    s->mask     = s->desc->slot_count - 1;
    s->last_seq = 0;

    DEBUG_PRINT_RING("subscriber ready\n");
}

/* =============================================================================
 * SUBSCRIBER NEXT MESSAGE
 * =============================================================================
 *
 * usrl_sub_next()
 *
 * Reads the next available message, if any. This is a very tight,
 * low-latency zero-copy consumer.
 *
 * Steps:
 *   1. Load w_head (writer head).
 *   2. Compute next sequence number subscriber expects.
 *   3. Handle overflow/skips if subscriber fell behind (FIX #3).
 *   4. Load the slot; check seq ordering.
 *   5. Copy payload out.
 *   6. Verify seq again (FIX #5: detect torn reads).
 *
 * Returns:
 *   >0 = payload length
 *    0 = no new message
 *   -1 = bad input
 *   -3 = subscriber-provided buffer too small
 *
 * =============================================================================
 */
int usrl_sub_next(
    UsrlSubscriber *s,
    uint8_t        *out_buf,
    uint32_t        buf_len,
    uint16_t       *out_pub_id
) {
    if (USRL_UNLIKELY(!s || !s->desc || !out_buf))
        return -1;

    RingDesc *d = s->desc;

    /* Writer head (last committed sequence) */
    uint64_t w_head =
        atomic_load_explicit(&d->w_head, memory_order_acquire);

    uint64_t next = s->last_seq + 1;

    /* ----------------------------------------------------------------------
     * Nothing new to read
     * ---------------------------------------------------------------------- */
    if (next > w_head)
        return 0;

    /* ----------------------------------------------------------------------
     * FIX #3: Subscriber falling behind
     *
     * If the gap > slot_count, writers have overwritten data we wanted.
     * Jump forward to the oldest still-valid message.
     * ---------------------------------------------------------------------- */
    if (w_head - next >= d->slot_count) {
        uint64_t new_start = w_head - d->slot_count + 1;

        /* Jump forward */
        s->last_seq = new_start - 1;
        next        = new_start;

        /* Re-check after alignment */
        w_head =
            atomic_load_explicit(&d->w_head, memory_order_acquire);

        if (next > w_head)
            return 0;
    }

    /* ----------------------------------------------------------------------
     * Index + Load slot
     * ---------------------------------------------------------------------- */
    uint32_t idx   = (uint32_t)((next - 1) & s->mask);
    uint8_t *slot  = s->base_ptr + ((uint64_t)idx * d->slot_size);
    SlotHeader *hdr = (SlotHeader*)slot;

    /* FIX #13: Prefetch next slot for reading */
    uint32_t next_idx = (uint32_t)(next & s->mask);
    USRL_PREFETCH_R(s->base_ptr + ((uint64_t)next_idx * d->slot_size));

    uint64_t seq =
        atomic_load_explicit(&hdr->seq, memory_order_acquire);

    /* Slot not written yet */
    if (seq == 0 || seq < next)
        return 0;

    /* ----------------------------------------------------------------------
     * FIX #3 & #9: Writer jumped ahead
     *
     * If seq > next, it means the writer lapped us while we were processing.
     * Adjust and return 0 to retry on next call.
     * ---------------------------------------------------------------------- */
    if (seq > next) {
        s->last_seq = seq - 1;
        return 0;
    }

    /* ----------------------------------------------------------------------
     * Safe to read payload
     * ---------------------------------------------------------------------- */
    uint32_t payload_len = hdr->payload_len;

    if (USRL_UNLIKELY(payload_len > buf_len)) {
        s->last_seq = next;
        return -3; /* buffer too small */
    }

    memcpy(out_buf, slot + sizeof(SlotHeader), payload_len);

    /* Return publisher ID if requested */
    if (out_pub_id)
        *out_pub_id = hdr->pub_id;

    /* ----------------------------------------------------------------------
     * FIX #5: Verify seq again (Optimistic Read / Seqlock pattern)
     *
     * If the writer overwrote this slot DURING our memcpy, the seq will
     * have changed. Discard the corrupted data and force jump to latest.
     * ---------------------------------------------------------------------- */
    atomic_thread_fence(memory_order_acquire);
    uint64_t post_seq =
        atomic_load_explicit(&hdr->seq, memory_order_relaxed);

    if (USRL_UNLIKELY(post_seq != seq)) {
        /* Writer lapped us during read - discard this frame */
        s->last_seq = w_head;
        return 0;
    }

    s->last_seq = next;
    return (int)payload_len;
}
