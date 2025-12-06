#include "usrl_ring.h"
#include "usrl_core.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

// define a unique ID for this publisher
#define MY_PUB_ID 100

int main(void) {
    // map the core memory
    void *core = usrl_core_map("/usrl_core", 32 * 1024 * 1024);
    if (!core) {
        printf("[PUB] Failed to map core. Did you run ./init_core?\n");
        return 1;
    }

    UsrlPublisher pub;
    memset(&pub, 0, sizeof(pub));

    // initialize with our ID
    // topic "demo" must exist in usrl_config.json
    usrl_pub_init(&pub, core, "demo", MY_PUB_ID);

    if (!pub.desc) {
        printf("[PUB] Failed to init (topic 'demo' missing?)\n");
        return 1;
    }

    printf("[PUB] ID %d initialized on 'demo'.\n", MY_PUB_ID);

    char msg[128];
    long count = 0;

    while (1) {
        snprintf(msg, sizeof(msg), "Hello World #%ld from ID %d", count++, MY_PUB_ID);

        int r = usrl_pub_publish(&pub, msg, (uint32_t)strlen(msg) + 1);

        if (r == 0) {
            // print every 1000th message to avoid spamming console
            if (count % 1000 == 0) {
                 printf("[PUB] Sent: %s\n", msg);
                 fflush(stdout);
            }
        } else {
            printf("[PUB] Error %d (payload too big?)\n", r);
        }

        // 1000 Hz rate (approx)
        usleep(1000);
    }

    return 0;
}
