#pragma once
#include <stdint.h>
__attribute__((noinline)) static int ds_control(const uint8_t *data, uint32_t size);
__attribute__((noinline)) static void ds_after_ack(const uint8_t *data, uint32_t size);
__attribute__((noinline)) static void ds_cleanup(void);
#define CFW_MESSAGE_AFTER_ACK ds_after_ack
