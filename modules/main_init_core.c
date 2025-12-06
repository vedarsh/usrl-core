#include "usrl_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CONFIG_TOPICS 64
#define CONFIG_FILE "usrl_config.json"

// (Reuse helper functions find_key, parse_string_val, parse_int_val from previous turn)
char* skip_ws(char *p) { while (*p && (*p <= 32)) p++; return p; }

char* find_key(char *json, const char *key) {
    char search[64]; sprintf(search, "\"%s\"", key);
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
    while (*p && *p != '\"' && i < max - 1) dest[i++] = *p++;
    dest[i] = 0;
}

int parse_int_val(char *p) { return atoi(p); }

int main(void) {
    printf("[INIT] Reading config from %s\n", CONFIG_FILE);
    FILE *f = fopen(CONFIG_FILE, "rb");
    if (!f) { printf("Failed to open config\n"); return 1; }
    fseek(f, 0, SEEK_END); long fsize = ftell(f); fseek(f, 0, SEEK_SET);
    char *buffer = malloc(fsize + 1); fread(buffer, 1, fsize, f); buffer[fsize] = 0; fclose(f);

    UsrlTopicConfig topics[MAX_CONFIG_TOPICS];
    int count = 0;
    char *p = strstr(buffer, "\"topics\"");

    while ((p = strchr(p, '{')) != NULL) {
        if (count >= MAX_CONFIG_TOPICS) break;
        char *name_p = find_key(p, "name");
        char *slots_p = find_key(p, "slots");
        char *size_p = find_key(p, "payload_size");
        char *type_p = find_key(p, "type"); // check for mwmr

        if (name_p && slots_p && size_p) {
            parse_string_val(name_p, topics[count].name, USRL_MAX_TOPIC_NAME);
            topics[count].slot_count = parse_int_val(slots_p);
            topics[count].slot_size  = parse_int_val(size_p);
            topics[count].type       = USRL_RING_TYPE_SWMR; // default

            if (type_p) {
                char type_str[16] = {0};
                parse_string_val(type_p, type_str, 16);
                if (strstr(type_str, "mwmr")) topics[count].type = USRL_RING_TYPE_MWMR;
            }

            printf("  Topic: %-20s Slots: %-5d Size: %-5d Type: %s\n",
                   topics[count].name, topics[count].slot_count, topics[count].slot_size,
                   topics[count].type == USRL_RING_TYPE_MWMR ? "MWMR" : "SWMR");
            count++;
        }
        p++;
    }
    free(buffer);

    if (usrl_core_init("/usrl_core", 32 * 1024 * 1024, topics, count) == 0) // 32MB
        printf("[INIT] Core initialized successfully.\n");
    else
        printf("[INIT] FAILED.\n");

    return 0;
}
