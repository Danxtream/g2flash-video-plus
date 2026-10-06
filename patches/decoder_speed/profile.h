#pragma once
#include <stdint.h>

/* Profiling is deliberately separate from the uninstrumented speed curve. */
#define DS_PROFILE_FUNCTIONS 512U
#define DS_PROFILE_DEPTH 64U
typedef struct { uint64_t inclusive, exclusive; uint32_t calls; } ds_profile_row;
typedef struct { uint32_t id, cycle, tick, child; } ds_profile_entry;
typedef struct {
    ds_profile_row rows[DS_PROFILE_FUNCTIONS];
    ds_profile_entry stack[DS_PROFILE_DEPTH];
    uint32_t count, depth, highwater, flags;
} ds_profile;
_Static_assert(sizeof(ds_profile)<=16384, "bounded profile scratch");
static void ds_profile_enter_at(ds_profile *p, uint32_t id, uint32_t cycle, uint32_t tick) {
    if (!p || p->flags) return;
    if (id>=p->count || p->depth==DS_PROFILE_DEPTH) { p->flags|=1; return; }
    p->stack[p->depth++]=(ds_profile_entry){id,cycle,tick,0};
    if (p->depth>p->highwater) p->highwater=p->depth;
}
static void ds_profile_exit_at(ds_profile *p, uint32_t id, uint32_t cycle, uint32_t tick) {
    if (!p || p->flags) return;
    if (!p->depth || p->stack[p->depth-1].id!=id) { p->flags|=2; return; }
    ds_profile_entry e=p->stack[--p->depth];
    uint32_t elapsed=cycle-e.cycle;
    /* 8 s at the accepted maximum 500 MHz cannot span a full CYCCNT wrap. */
    if (tick-e.tick>=8000 || e.child>elapsed) { p->flags|=4; return; }
    ds_profile_row *r=&p->rows[id];
    if (r->calls==0xffffffffU) { p->flags|=8; return; }
    ++r->calls; r->inclusive+=elapsed; r->exclusive+=elapsed-e.child;
    if (p->depth) {
        uint32_t *child=&p->stack[p->depth-1].child;
        if (*child>0xffffffffU-elapsed) { p->flags|=8; return; }
        *child+=elapsed;
    }
}
