#pragma once
#include <stdint.h>
__attribute__((noinline)) static int ds_control(const uint8_t *data, uint32_t size);
__attribute__((noinline)) static void ds_after_ack(const uint8_t *data, uint32_t size);
__attribute__((noinline)) static void ds_cleanup(void);
#define CFW_MESSAGE_AFTER_ACK ds_after_ack
/* Keep mode 31 local to exactly one lens; other private messages retain routing. */
static int ds_direct_message(const uint8_t *data, uint32_t size, uint8_t here,
                             uint8_t origin, uint8_t target) {
    return !(size && data[0]==31) || (here==origin && target==here);
}
