#include "usrl_core.h"
#include "usrl_ring.h"
#include <string.h>
#include <time.h>

#ifdef DEBUG
    #define DEBUG_PRINT_RING(...) \
        do { printf("[DEBUG][RING] " __VA_ARGS__); fflush(stdout); } while (0)
#else
    #define DEBUG_PRINT_RING(...) ((void)0)
#endif

// added pub_id to init
void usrl_pub_init(UsrlPublisher *p, void *core_base, const char *topic, uint16_t pub_id) {
    if (!p || !core_base || !topic) return;

    TopicEntry *t = usrl_get_topic(core_base, topic);
    if (!t) {
        DEBUG_PRINT_RING("could not find topic\n");
        return;
    }

    p->desc     = (RingDesc*)((uint8_t*)core_base + t->ring_desc_offset);
    p->base_ptr = (uint8_t*)core_base + p->desc->base_offset;
    p->mask     = p->desc->slot_count - 1;
    p->pub_id   = pub_id; // save the id

    DEBUG_PRINT_RING("publisher %u ready\n", pub_id);
}

int usrl_pub_publish(UsrlPublisher *p, const void *data, uint32_t len) {
    if (!p || !p->desc || !data) return -1;

    RingDesc *d = p->desc;

    if (len > (d->slot_size - sizeof(SlotHeader))) return -2;

    // ATOMIC RESERVATION: safe for multiple writers
    // fetch_add returns unique index for each caller
    uint64_t old_head  = atomic_fetch_add_explicit(&d->w_head, 1, memory_order_acq_rel);
    uint64_t commit_seq = old_head + 1;

    uint32_t idx = (uint32_t)((commit_seq - 1) & p->mask);

    uint8_t    *slot = p->base_ptr + ((uint64_t)idx * d->slot_size);
    SlotHeader *hdr  = (SlotHeader*)slot;

    memcpy(slot + sizeof(SlotHeader), data, len);
    hdr->payload_len = len;
    hdr->pub_id      = p->pub_id; // write our id

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    hdr->timestamp_ns = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;

    atomic_store_explicit(&hdr->seq, commit_seq, memory_order_release);

    DEBUG_PRINT_RING("pub %u wrote seq %lu\n", p->pub_id, commit_seq);
    return 0;
}

void usrl_sub_init(UsrlSubscriber *s, void *core_base, const char *topic) {
    if (!s || !core_base || !topic) return;

    TopicEntry *t = usrl_get_topic(core_base, topic);
    if (!t) return;

    s->desc     = (RingDesc*)((uint8_t*)core_base + t->ring_desc_offset);
    s->base_ptr = (uint8_t*)core_base + s->desc->base_offset;
    s->mask     = s->desc->slot_count - 1;
    s->last_seq = 0;

    DEBUG_PRINT_RING("subscriber ready\n");
}

int usrl_sub_next(UsrlSubscriber *s, uint8_t *out_buf, uint32_t buf_len, uint16_t *out_pub_id) {
    if (!s || !s->desc || !out_buf) return -1;

    RingDesc *d = s->desc;
    uint64_t w_head = atomic_load_explicit(&d->w_head, memory_order_acquire);
    uint64_t next   = s->last_seq + 1;

    if (next > w_head) return 0;

    if (w_head - next >= d->slot_count) {
        uint64_t new_start = w_head - d->slot_count + 1;
        s->last_seq = new_start - 1;
        next = new_start;
        w_head = atomic_load_explicit(&d->w_head, memory_order_acquire);
        if (next > w_head) return 0;
    }

    uint32_t idx = (uint32_t)((next - 1) & s->mask);
    uint8_t  *slot = s->base_ptr + ((uint64_t)idx * d->slot_size);
    SlotHeader *hdr = (SlotHeader*)slot;

    uint64_t seq = atomic_load_explicit(&hdr->seq, memory_order_acquire);

    if (seq == 0 || seq < next) return 0;

    if (seq > next) {
        s->last_seq = seq - 1;
        return 0;
    }

    uint32_t payload_len = hdr->payload_len;

    if (payload_len > buf_len) {
        s->last_seq = next;
        return -3;
    }

    memcpy(out_buf, slot + sizeof(SlotHeader), payload_len);

    // give the ID back to the caller if they asked for it
    if (out_pub_id) {
        *out_pub_id = hdr->pub_id;
    }

    s->last_seq = next;
    return (int)payload_len;
}
