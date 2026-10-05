#pragma once

#ifdef DS_HOST_TEST
#include "host_platform.h"
#else
#include "../cfw_context.h"
#include "../malloc.h"
#include "decoder_speed_capsule.h"

/* Entry points are pinned to the audited 2.2.9.22 donor in abi.py. */
#define DS_THREAD_NEW ((uint32_t (*)(void (*)(void *), void *, const void *))0x00442897U)
#define DS_THREAD_TERMINATE ((int (*)(uint32_t))0x004429b3U)
#define DS_DELAY ((int (*)(uint32_t))0x00442b2bU)
#define DS_TIMER_NEW ((uint32_t (*)(void (*)(void *), uint32_t, void *, void *))0x00442b65U)
#define DS_TIMER_START ((int (*)(uint32_t, uint32_t))0x00442c4dU)
#define DS_TIMER_STOP ((int (*)(uint32_t))0x00442c8dU)
#define DS_TIMER_DELETE ((int (*)(uint32_t))0x00442cf3U)
#define DS_MUTEX_TAKE ((int (*)(uint32_t, uint32_t))0x00442f91U)
#define DS_MUTEX_GIVE ((int (*)(uint32_t))0x00442ff7U)
#define DS_BLE_SEND ((int (*)(uint8_t, uint8_t, const uint8_t *, uint16_t))0x0047d72dU)
#define DS_SIDE ((uint32_t (*)(void))0x0045cfddU)
#define DS_REG(addr) (*(volatile uint32_t *)(addr))
static inline void ds_read_control(uint32_t *control, uint32_t *ipsr) {
    __asm__ volatile("mrs %0, control\n mrs %1, ipsr" : "=r"(*control), "=r"(*ipsr));
}
static inline void ds_barrier(int instruction) {
    __asm__ volatile("dsb" ::: "memory");
    if (instruction) __asm__ volatile("isb" ::: "memory");
}
static inline void ds_nop(void) { __asm__ volatile("nop"); }
#endif
