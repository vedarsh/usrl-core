#include "usrl_core.h"
#include "usrl_ring.h"
#include <string.h>
#include <time.h>
#include <sched.h>

// simple backoff for contention
static void backoff(int iter) {
    if (iter < 10) {
        // spin
        __asm__ volatile("nop");
    } else {
        sched_yield();
    }
}

void usrl_mwmr_pub_init(UsrlMwmrPublisher *p, void *core_base, const char *topic, uint16_t pub_id) {
    if (!p || !core_base || !topic) return;

    TopicEntry *t = usrl_get_topic(core_base, topic);
    if (!t) return;

    // Check type safety
    if (t->type != USRL_RING_TYPE_MWMR) {
        printf("[ERROR] Topic '%s' is not MWMR!\n", topic);
        return;
    }

    p->desc     = (RingDesc*)((uint8_t*)core_base + t->ring_desc_offset);
    p->base_ptr = (uint8_t*)core_base + p->desc->base_offset;
    p->mask     = p->desc->slot_count - 1;
    p->pub_id   = pub_id;
}

int usrl_mwmr_pub_publish(UsrlMwmrPublisher *p, const void *data, uint32_t len) {
    if (!p || !p->desc || !data) return -1;

    RingDesc *d = p->desc;
    if (len > (d->slot_size - sizeof(SlotHeader))) return -2;

    // 1. ATOMIC RESERVATION
    // This is the contention point. Multiple threads/processes hit this.
    // We get a unique monotonic 'index' (old_head).
    uint64_t old_head = atomic_fetch_add_explicit(&d->w_head, 1, memory_order_acq_rel);
    uint64_t commit_seq = old_head + 1;
    uint32_t idx = (uint32_t)((commit_seq - 1) & p->mask);

    uint8_t    *slot = p->base_ptr + ((uint64_t)idx * d->slot_size);
    SlotHeader *hdr  = (SlotHeader*)slot;

    // 2. SAFETY CHECK (Critical for MWMR)
    // In a ring, we might be overtaking the reader TAIL or an old writer.
    // We need to make sure the slot we just reserved isn't currently being written to
    // by a *lagging* writer from a previous wrap-around.

    // Ideally, we check 'seq'.
    // If seq == 0, slot is empty.
    // If seq < commit_seq - slot_count, it's old data, safe to overwrite.
    // If seq > commit_seq, something is very broken.
    // If seq is "in progress" (we define a marker), we wait.

    // For this implementation, we rely on the fact that 'seq' is only updated
    // at the END of the write.
    // However, there is a risk: What if Writer A reserves Slot 1 (Seq 100)
    // but hasn't started writing, and Writer B (Seq 100 + SlotCount) wraps around
    // and tries to overwrite Slot 1?
    // Writer B sees old Seq (e.g. 0 or previous).
    // Writer B writes. Writer A writes. Collision.

    // Spin wait until the slot is ready to be overwritten.
    // The slot is ready if its current `seq` belongs to the previous generation.
    // Expected previous seq = commit_seq - slot_count.
    // Initial case: seq = 0.

    int iter = 0;
    while (1) {
        uint64_t current_seq = atomic_load_explicit(&hdr->seq, memory_order_acquire);
        uint64_t diff = commit_seq - current_seq;

        // If diff == slot_count + 1, it means the slot contains exactly the message
        // from one full lap ago. It is fully committed and safe to overwrite.
        // Special case: if current_seq == 0, it's never been used.
        if (current_seq == 0 || diff >= d->slot_count) {
            break; // Safe to write
        }

        // Backoff if the reader hasn't consumed it or previous writer is slow
        backoff(iter++);
        if (iter > 10000) return -3; // Timeout / Deadlock avoidance
    }

    // 3. WRITE DATA
    memcpy(slot + sizeof(SlotHeader), data, len);
    hdr->payload_len = len;
    hdr->pub_id      = p->pub_id;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    hdr->timestamp_ns = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;

    // 4. COMMIT
    atomic_store_explicit(&hdr->seq, commit_seq, memory_order_release);
    return 0;
}
