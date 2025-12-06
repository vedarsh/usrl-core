#ifndef USRL_RING_H
#define USRL_RING_H

#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include "usrl_core.h"

typedef struct {
    RingDesc *desc;
    uint8_t  *base_ptr;
    uint32_t  mask;
    uint16_t  pub_id;
} UsrlPublisher;

typedef struct {
    RingDesc *desc;
    uint8_t  *base_ptr;
    uint32_t  mask;
    uint64_t  last_seq;
} UsrlSubscriber;

// MWMR Publisher Handle
typedef struct {
    RingDesc *desc;
    uint8_t  *base_ptr;
    uint32_t  mask;
    uint16_t  pub_id;
} UsrlMwmrPublisher;

// MWMR API
void usrl_mwmr_pub_init(UsrlMwmrPublisher *p, void *core_base, const char *topic, uint16_t pub_id);
int  usrl_mwmr_pub_publish(UsrlMwmrPublisher *p, const void *data, uint32_t len);
void usrl_mwmr_sub_init(UsrlSubscriber *s, void *core_base, const char *topic);

// pass pub_id here
void usrl_pub_init(UsrlPublisher *p, void *core_base, const char *topic, uint16_t pub_id);
int  usrl_pub_publish(UsrlPublisher *p, const void *data, uint32_t len);

void usrl_sub_init(UsrlSubscriber *s, void *core_base, const char *topic);
// subscriber can now ask "who sent this?" via out_pub_id
int  usrl_sub_next(UsrlSubscriber *s, uint8_t *out_buf, uint32_t buf_len, uint16_t *out_pub_id);

#endif
