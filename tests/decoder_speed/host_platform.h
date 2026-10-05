#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint32_t image_mutex; void *decoder_speed_session; } customCfwContext;
typedef struct { uint32_t free_bytes, max_alloc; } cfw_heap_stats;
#define TLSF_FREE_INVALID 0xffffffffU
#define FW_HEAP_13_DESCRIPTOR 0x20000358U
static customCfwContext context={1,0};
static uint32_t tick, allocations, releases, allocation_attempts, fail_at;
static uint32_t thread_new_fail, terminated, timer_fail, timer_deleted, mutex_busy;
static uint32_t regs[16], mpu_base, mpu_limit, mpu_mair;
static uint8_t reply[10];
static void *import_slot;
#define DS_IMPORT_SLOT ((uintptr_t)&import_slot)
#define FW_MS_TICK tick
#define DS_CAPSULE_BYTES 136792U
#define DS_OFFSET_DS_SELFTEST 0
#define DS_OFFSET_DS_SIZE 0
#define DS_OFFSET_DS_INIT 0
#define DS_OFFSET_DS_DESTROY 0
#define DS_OFFSET_DS_DECODE 0
#define DS_OFFSET_DS_FRAME 0
static const uint8_t ds_capsule_image[DS_CAPSULE_BYTES]={0};
static customCfwContext *getCustomCfwContext(void) { return &context; }
static customCfwContext *peekCustomCfwContext(void) { return &context; }
static void host_zero(uint8_t *p,uint32_t n) { memset(p,0,n); }
#define bzero host_zero
static void *host_alloc(uint32_t n) {
    if (++allocation_attempts==fail_at) return 0;
    void *p=malloc(n); if (p) ++allocations; return p;
}
static void host_free(void *p) { if (p) { ++releases; free(p); } }
static void *host_heap_alloc(uint32_t heap,uint32_t n) { (void)heap; return host_alloc(n); }
static void host_heap_free(uint32_t heap,void *p) { (void)heap; host_free(p); }
#define FW_MALLOC host_alloc
#define FW_FREE host_free
#define FW_HEAP_MALLOC host_heap_alloc
#define FW_HEAP_FREE host_heap_free
static cfw_heap_stats ds_host_stats(uint32_t heap) {
    cfw_heap_stats s={heap==27 ? 172620U : heap==13 ? 561592U : 428028U,
        heap==27 ? 172596U : heap==13 ? 561544U : 428028U};
    return s;
}
static uint32_t host_thread_new(void (*fn)(void *),void *arg,const void *attr) {
    (void)fn; (void)arg; (void)attr; return thread_new_fail ? 0 : 1;
}
static int host_terminate(uint32_t thread) { (void)thread; ++terminated; return 0; }
static int host_delay(uint32_t n) { tick+=n; return 0; }
static uint32_t host_timer_new(void (*fn)(void *),uint32_t kind,void *arg,void *attr) {
    (void)fn; (void)kind; (void)arg; (void)attr; return timer_fail ? 0 : 1;
}
static int host_timer_start(uint32_t timer,uint32_t n) { (void)timer; (void)n; return 0; }
static int host_timer_stop(uint32_t timer) { (void)timer; return timer_fail ? -1 : 0; }
static int host_timer_delete(uint32_t timer) { (void)timer; ++timer_deleted; return 0; }
static int host_mutex_take(uint32_t mutex,uint32_t timeout) { (void)mutex; (void)timeout; return mutex_busy ? -1 : 0; }
static int host_mutex_give(uint32_t mutex) { (void)mutex; return 0; }
static int host_send(uint8_t service,uint8_t channel,const uint8_t *p,uint16_t n) {
    (void)service; (void)channel; if(n!=10) abort(); memcpy(reply,p,n); return 0;
}
static uint32_t host_side(void) { return 2; }
#define DS_THREAD_NEW host_thread_new
#define DS_THREAD_TERMINATE host_terminate
#define DS_DELAY host_delay
#define DS_TIMER_NEW host_timer_new
#define DS_TIMER_START host_timer_start
#define DS_TIMER_STOP host_timer_stop
#define DS_TIMER_DELETE host_timer_delete
#define DS_MUTEX_TAKE host_mutex_take
#define DS_MUTEX_GIVE host_mutex_give
#define DS_BLE_SEND host_send
#define DS_SIDE host_side
static uint32_t *host_reg(uint32_t addr) {
    switch(addr) {
    case 0xe000ed90U: return &regs[0];
    case 0xe000ed94U: return &regs[1];
    case 0xe000ed98U: return &regs[2];
    case 0xe000ed9cU: return &mpu_base;
    case 0xe000eda0U: return &mpu_limit;
    case 0xe000edc0U: return &mpu_mair;
    case 0xe000ed14U: return &regs[3];
    case 0xe000ed7cU: return &regs[4];
    case 0xe000edfcU: return &regs[5];
    case 0xe0001000U: return &regs[6];
    default: return &regs[7];
    }
}
#define DS_REG(addr) (*host_reg(addr))
static void ds_read_control(uint32_t *control,uint32_t *ipsr) { *control=regs[8]; *ipsr=regs[9]; }
static void ds_barrier(int instruction) { (void)instruction; }
static void ds_nop(void) { ++tick; }
