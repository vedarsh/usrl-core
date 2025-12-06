#include "usrl_ring.h"
#include "usrl_core.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    void *core = usrl_core_map("/usrl_core", 32 * 1024 * 1024);
    if (!core) {
        printf("[SUB] Failed to map core.\n");
        return 1;
    }

    UsrlSubscriber sub;
    memset(&sub, 0, sizeof(sub));

    // subscribe to "demo"
    usrl_sub_init(&sub, core, "demo");

    if (!sub.desc) {
        printf("[SUB] Failed to init (topic 'demo' missing?)\n");
        return 1;
    }

    printf("[SUB] Listening on 'demo'...\n");

    uint8_t buf[1024]; // ensure this is larger than your max payload
    uint16_t pub_id = 0;
    long count = 0;

    while (1) {
        // pass &pub_id to know who sent it
        int n = usrl_sub_next(&sub, buf, sizeof(buf), &pub_id);

        if (n > 0) {
            buf[n] = 0; // null terminate to print as string
            count++;

            // print every 1000th message
            if (count % 1000 == 0) {
                printf("[SUB] Received from ID %d: %s\n", pub_id, (char*)buf);
                fflush(stdout);
            }
            continue;
        }

        if (n == 0) {
            // no data, sleep briefly
            usleep(100);
            continue;
        }

        if (n == -3) {
             printf("[SUB] Buffer too small for message!\n");
        }
    }
    return 0;
}
