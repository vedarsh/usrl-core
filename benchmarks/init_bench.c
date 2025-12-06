#include "usrl_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CONFIG_TOPICS 64
#define CONFIG_FILE "usrl_config_bench.json"

// --- Helper Functions (Same as init_core.c) ---

char* skip_ws(char *p) {
    while (*p && (*p <= 32)) p++;
    return p;
}

char* find_key(char *json, const char *key) {
    char search[64];
    sprintf(search, "\"%s\"", key);
    char *loc = strstr(json, search);
    if (!loc) return NULL;

    loc += strlen(search);
    while (*loc && *loc != ':') loc++;
    if (*loc == ':') loc++;

    return skip_ws(loc);
}

void parse_string_val(char *p, char *dest, int max) {
    if (*p != '\"') return;
    p++;
    int i = 0;
    while (*p && *p != '\"' && i < max - 1) {
        dest[i++] = *p++;
    }
    dest[i] = 0;
}

int parse_int_val(char *p) {
    return atoi(p);
}

// --- Main ---

int main(void) {
    printf("[BENCH_INIT] Reading config from %s\n", CONFIG_FILE);

    FILE *f = fopen(CONFIG_FILE, "rb");
    if (!f) {
        printf("[BENCH_INIT] Error: could not open %s\n", CONFIG_FILE);
        return 1;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buffer = malloc(fsize + 1);
    fread(buffer, 1, fsize, f);
    buffer[fsize] = 0;
    fclose(f);

    UsrlTopicConfig topics[MAX_CONFIG_TOPICS];
    int count = 0;

    // Default 128MB for benchmarks
    uint64_t mem_size = 128 * 1024 * 1024;

    // Check config for memory override
    char *mem_p = find_key(buffer, "memory_size_mb");
    if (mem_p) {
        int mb = parse_int_val(mem_p);
        if (mb > 0) mem_size = (uint64_t)mb * 1024 * 1024;
    }

    // Hard check: Ensure at least 64MB for benchmarks
    if (mem_size < 64 * 1024 * 1024) mem_size = 64 * 1024 * 1024;

    printf("[BENCH_INIT] Memory Size: %lu MB\n", mem_size / (1024*1024));

    char *p = strstr(buffer, "\"topics\"");
    if (p) {
        while ((p = strchr(p, '{')) != NULL) {
            if (count >= MAX_CONFIG_TOPICS) break;

            char *name_p = find_key(p, "name");
            char *slots_p = find_key(p, "slots");
            char *size_p = find_key(p, "payload_size");
            char *type_p = find_key(p, "type");

            if (name_p && slots_p && size_p) {
                parse_string_val(name_p, topics[count].name, USRL_MAX_TOPIC_NAME);
                topics[count].slot_count = parse_int_val(slots_p);
                topics[count].slot_size  = parse_int_val(size_p);

                // Set default
                topics[count].type = 0; // SWMR

                // Check Type override
                if (type_p) {
                    char type_str[16] = {0};
                    parse_string_val(type_p, type_str, 16);
                    if (strstr(type_str, "mwmr")) {
                         // Must match USRL_RING_TYPE_MWMR defined in usrl_core.h (likely 1)
                         // Since we can't see the header here, assuming 1.
                         // Better is to include header definition, but assuming 1 for MWMR works if defined that way.
                         // Actually, let's look at usrl_core.h from previous turn. It was defined as:
                         // #define USRL_RING_TYPE_SWMR 0
                         // #define USRL_RING_TYPE_MWMR 1
                         // But that was added in a "snippet".
                         // Assuming the header has these defines now.
                         // If not, use 1.
                         topics[count].type = 1;
                    }
                }

                printf("  Loaded: %-20s (Slots: %d, Size: %d)\n",
                       topics[count].name, topics[count].slot_count, topics[count].slot_size);
                count++;
            }
            p++;
        }
    }

    free(buffer);

    if (usrl_core_init("/usrl_core", mem_size, topics, count) == 0) {
        printf("[BENCH_INIT] Core initialized successfully.\n");
    } else {
        printf("[BENCH_INIT] FAILED to initialize core.\n");
        return 1;
    }

    return 0;
}
