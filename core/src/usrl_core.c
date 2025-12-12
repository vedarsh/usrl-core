#include "usrl_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <errno.h>

/**
 * @file usrl_core.c
 * @brief Core routines for creating and mapping the USRL shared memory region.
 */

/* --------------------------------------------------------------------------
 * Debug Helpers
 * -------------------------------------------------------------------------- */
#ifdef DEBUG
#define DEBUG_PRINT_CORE(...)                 \
    do                                        \
    {                                         \
        printf("[DEBUG][CORE] " __VA_ARGS__); \
        fflush(stdout);                       \
    } while (0)
#else
#define DEBUG_PRINT_CORE(...) ((void)0)
#endif

static uint32_t next_power_of_two_u32(uint32_t v)
{
    if (v == 0) return 1;
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    return ++v;
}

/**
 * usrl_core_init return codes (recommended convention)
 *  0  : created and initialized
 *  1  : already exists (not initialized by this call)
 * -1  : invalid params or shm_open failed (not EEXIST)
 * -2  : ftruncate failed
 * -3  : mmap failed
 * -4  : insufficient SHM space for requested layout
 */
int usrl_core_init(
    const char *path,
    uint64_t size,
    const UsrlTopicConfig *topics,
    uint32_t count)
{
    DEBUG_PRINT_CORE("init path=%s size=%llu topics=%u\n",
                     path, (unsigned long long)size, count);

    if (!path || size < 4096 || !topics || count == 0) return -1;

    /* Create fresh shared memory object only if it does NOT exist */
    int fd = shm_open(path, O_CREAT | O_RDWR | O_EXCL, 0666);
    if (fd < 0) {
        if (errno == EEXIST) {
            /* Not an error: someone already created it. */
            return 1;
        }
        DEBUG_PRINT_CORE("shm_open failed errno=%d\n", errno);
        return -1;
    }

    if (ftruncate(fd, (off_t)size) < 0) {
        DEBUG_PRINT_CORE("ftruncate failed errno=%d\n", errno);
        close(fd);
        /* object exists but is unusable; caller may decide to shm_unlink() */
        return -2;
    }

    void *base = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        DEBUG_PRINT_CORE("mmap failed errno=%d\n", errno);
        close(fd);
        return -3;
    }

    memset(base, 0, size);

    CoreHeader *hdr = (CoreHeader *)base;
    hdr->magic = USRL_MAGIC;
    hdr->version = 1;
    hdr->mmap_size = size;

    uint64_t current_offset = usrl_align_up(sizeof(CoreHeader), USRL_ALIGNMENT);

    hdr->topic_table_offset = current_offset;
    hdr->topic_count = count;

    uint64_t ring_desc_start = usrl_align_up(
        current_offset + (sizeof(TopicEntry) * count),
        USRL_ALIGNMENT);

    uint64_t slots_start = usrl_align_up(
        ring_desc_start + (sizeof(RingDesc) * count),
        USRL_ALIGNMENT);

    uint64_t next_free_slot_offset = slots_start;

    for (uint32_t i = 0; i < count; ++i) {
        TopicEntry *t = (TopicEntry *)((uint8_t *)base +
                                       hdr->topic_table_offset +
                                       (i * sizeof(TopicEntry)));

        strncpy(t->name, topics[i].name, USRL_MAX_TOPIC_NAME - 1);
        t->name[USRL_MAX_TOPIC_NAME - 1] = '\0';

        t->ring_desc_offset = ring_desc_start + (i * sizeof(RingDesc));

        uint32_t slots_pow2 = next_power_of_two_u32(topics[i].slot_count);
        uint32_t slot_sz_aligned =
            (uint32_t)usrl_align_up(sizeof(SlotHeader) + topics[i].slot_size, 8);

        t->type = topics[i].type;
        t->slot_count = slots_pow2;
        t->slot_size = slot_sz_aligned;

        RingDesc *r = (RingDesc *)((uint8_t *)base + t->ring_desc_offset);
        r->slot_count = slots_pow2;
        r->slot_size = slot_sz_aligned;
        r->base_offset = next_free_slot_offset;
        atomic_store_explicit(&r->w_head, 0, memory_order_relaxed);

        uint64_t total_bytes_for_topic = (uint64_t)slots_pow2 * slot_sz_aligned;

        if (next_free_slot_offset + total_bytes_for_topic > size) {
            DEBUG_PRINT_CORE("OOM topic=%s needs=%llu bytes\n",
                             topics[i].name,
                             (unsigned long long)total_bytes_for_topic);
            munmap(base, size);
            close(fd);
            return -4;
        }

        uint8_t *slot_ptr = (uint8_t *)base + next_free_slot_offset;
        for (uint32_t k = 0; k < slots_pow2; ++k) {
            SlotHeader *sh = (SlotHeader *)(slot_ptr + ((uint64_t)k * slot_sz_aligned));
            atomic_store_explicit(&sh->seq, 0, memory_order_relaxed);
        }

        next_free_slot_offset += total_bytes_for_topic;
        next_free_slot_offset = usrl_align_up(next_free_slot_offset, USRL_ALIGNMENT);
    }

    DEBUG_PRINT_CORE("used %llu / %llu bytes\n",
                     (unsigned long long)next_free_slot_offset,
                     (unsigned long long)size);

    munmap(base, size);
    close(fd);
    return 0;
}

/**
 * Map an existing region. If 'size' is 0 or too large, map the SHM object size.
 */
void *usrl_core_map(const char *path, uint64_t size)
{
    int fd = shm_open(path, O_RDWR, 0666);
    if (fd < 0) return NULL;

    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        return NULL;
    }

    uint64_t obj_size = (st.st_size > 0) ? (uint64_t)st.st_size : 0;
    if (obj_size == 0) {
        close(fd);
        return NULL;
    }

    uint64_t map_size = size;
    if (map_size == 0 || map_size > obj_size) map_size = obj_size;

    void *base = mmap(NULL, (size_t)map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        close(fd);
        return NULL;
    }

    close(fd);
    return base;
}

TopicEntry *usrl_get_topic(void *base, const char *name)
{
    if (!base || !name) return NULL;

    CoreHeader *hdr = (CoreHeader *)base;
    if (hdr->magic != USRL_MAGIC) return NULL;

    TopicEntry *t = (TopicEntry *)((uint8_t *)base + hdr->topic_table_offset);

    for (uint32_t i = 0; i < hdr->topic_count; i++) {
        if (strncmp(t[i].name, name, USRL_MAX_TOPIC_NAME) == 0)
            return &t[i];
    }
    return NULL;
}

void usrl_core_unmap(void *base, size_t size)
{
    if (base && size) munmap(base, size);
}
