#include "controller.h"
#pragma clang section text=".text.decoder_speed"
#include "pool.h"
#include "platform.h"
#include "clips.h"
#include "sha256.h"
#include "profile.h"
#define DS_CAPSULE_MAX_BYTES 156108U
#define DS_MAGIC 0x44530301U
#define DS_HEAP27 0x2000033cU
#define DS_HEADER_WORDS 128U
#define DS_FRAMES 32U
#define DS_FRAME_WORDS 5U
#define DS_WORDS (DS_HEADER_WORDS + DS_FRAMES*7U*DS_FRAME_WORDS)
#define DS_STACK_BYTES 16384U
#define DS_STATE_BYTES 4096U
#define DS_META_BYTES 3072U
#define DS_TABLE_BYTES 50160U
#define DS_COLOR_BYTES (122880U + DS_TABLE_BYTES + 61440U)
#define DS_LUMA_BYTES (122880U + DS_TABLE_BYTES)
#define DS_CYCLES DS_REG(0xe0001004U)

enum { DS_UPLOAD = 1, DS_SEALED, DS_RUNNING, DS_DONE, DS_FAILED, DS_CANCELLED };
enum { DS_HELLO = 0, DS_BEGIN, DS_WRITE, DS_SEAL, DS_RUN, DS_READ, DS_ABORT, DS_CLOSE, DS_HEAPS, DS_CAP_WRITE, DS_CAP_SEAL, DS_PROFILE_READ };
typedef struct {
    const char *name;
    uint32_t attr_bits;
    void *cb_mem; uint32_t cb_size;
    void *stack_mem; uint32_t stack_size;
    int32_t priority;
    uint32_t tz_module, reserved;
} ds_thread_attr;
typedef struct {
    ds_imports imports;
    ds_pool pool;
    volatile uint32_t phase, cancel, start, parked, profile_recording;
    uint32_t session, nonce, uploaded, code_uploaded, deadline, run_deadline, thread, timer;
    uint32_t clip_bytes, clip_crc;
    uint32_t setup, skip, error;
    uint32_t code_bytes;
    uint32_t offsets[6], text_lo, text_hi, kind, profile_count;
    uint32_t clip_sealed, code_sealed;
    uint8_t code_hash[32];
    ds_profile *profile;
    uint32_t old_demcr, old_dwt_ctrl, clock_owned;
    uint8_t *clip, *hot_raw, *state_raw, *stack_raw, *meta, *code_raw;
    uint8_t *hot, *state, *stack;
    const uint8_t *code;
    uint8_t tcb[112] __attribute__((aligned(8)));
    uint32_t words[DS_WORDS], used_words;
} ds_session;

static uint32_t ds_rd16(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8; }
static uint32_t ds_rd32(const uint8_t *p) { return ds_rd16(p) | ds_rd16(p+2) << 16; }
static uint8_t *ds_align(uint8_t *p, uint32_t n) {
    return (uint8_t *)(((uintptr_t)p+n-1) & ~(uintptr_t)(n-1));
}
static ds_session *ds_active(void) {
    customCfwContext *ctx = peekCustomCfwContext();
    return ctx ? (ds_session *)ctx->decoder_speed_session : 0;
}
static uint32_t ds_crc(const uint8_t *p, uint32_t n, uint32_t crc) {
    while (n--) {
        crc ^= *p++;
        for (uint32_t bit=0; bit<8; ++bit) crc = (crc >> 1) ^ (0xedb88320U & (0U-(crc & 1U)));
    }
    return crc;
}
static int ds_reply(uint32_t session, uint32_t index, uint32_t value) {
    uint32_t side = DS_SIDE();
    uint8_t body[10] = {5, side == 1 ? 2 : side == 2 ? 1 : 0,
        (uint8_t)session, (uint8_t)(session>>8), (uint8_t)index, (uint8_t)(index>>8),
        (uint8_t)value, (uint8_t)(value>>8), (uint8_t)(value>>16), (uint8_t)(value>>24)};
    return body[1] ? DS_BLE_SEND(1, 0xf0, body, sizeof(body)) : -1;
}
static cfw_heap_stats ds_stats(uint32_t heap) {
#ifdef DS_HOST_TEST
    return ds_host_stats(heap);
#else
    if (heap == 13) return heap_object_stats(FW_HEAP_13_DESCRIPTOR, 0x201350a8U, 0xcd000U);
    if (heap == 27) return heap_object_stats(DS_HEAP27, 0x202728a8U, 0x2cc00U);
    if (DS_REG(0x20076e08U) == 0x202020a8U) return tlsf_arena_stats(0x202020a8U, 0x70800U);
    const cfw_heap_stats invalid = {TLSF_FREE_INVALID, TLSF_FREE_INVALID};
    return invalid;
#endif
}
static int ds_room(uint32_t heap, uint32_t total, uint32_t largest) {
    cfw_heap_stats stats = ds_stats(heap);
    uint32_t reserve = heap == 27 ? 16384U : 32768U;
    return stats.free_bytes != TLSF_FREE_INVALID && stats.free_bytes >= total + reserve
        && stats.max_alloc >= largest;
}
static void ds_record_heaps(ds_session *s, uint32_t offset) {
    const uint32_t heaps[3] = {20, 13, 27};
    for (uint32_t i=0; i<3; ++i) {
        cfw_heap_stats stats = ds_stats(heaps[i]);
        s->words[offset+i*2] = stats.free_bytes;
        s->words[offset+i*2+1] = stats.max_alloc;
    }
}
static void ds_release_run(ds_session *s) {
    if (s->hot_raw) {
        FW_FREE(s->hot_raw);
        s->hot_raw = 0;
    }
    if (s->state_raw) { FW_FREE(s->state_raw); s->state_raw = 0; }
    if (s->meta) { FW_FREE(s->meta); s->meta = 0; }
    if (s->stack_raw) { FW_FREE(s->stack_raw); s->stack_raw = 0; }
    bzero((uint8_t *)&s->pool, sizeof(s->pool));
    if (s->profile) { FW_FREE(s->profile); s->profile=0; }
}
static void ds_park(ds_session *s) __attribute__((noreturn));
static void ds_park(ds_session *s) {
    __atomic_store_n(&s->parked, 1, __ATOMIC_RELEASE);
    for (;;) DS_DELAY(1000);
}
static void ds_fatal(uint32_t reason) __attribute__((noreturn));
static void ds_fatal(uint32_t reason) {
    ds_session *s = ds_active();
    s->error = 0x100U + reason;
    s->words[2] = s->error;
    s->words[3] = s->used_words;
    __atomic_store_n(&s->phase, DS_FAILED, __ATOMIC_RELEASE);
    ds_park(s);
}
static void ds_profile_push(uint32_t id) {
    ds_session *s=ds_active();
    if (s && s->kind==1 && s->profile_recording) ds_profile_enter_at(s->profile,id,DS_CYCLES,FW_MS_TICK);
}
static void ds_profile_pop(uint32_t id) {
    ds_session *s=ds_active();
    if (s && s->kind==1 && s->profile_recording) ds_profile_exit_at(s->profile,id,DS_CYCLES,FW_MS_TICK);
}
static void *ds_allocate(uint32_t n) { return ds_pool_alloc(&ds_active()->pool, n); }
static void ds_deallocate(void *p) {
    if (!ds_pool_free(&ds_active()->pool, p)) ds_fatal(6);
}
static int ds_preallocate(const ds_request *r, uint32_t n, ds_failure *f) {
    return ds_pool_preflight(&ds_active()->pool, r, n, f);
}
static int ds_terminate(ds_session *s) {
    if (!s->thread) return 1;
    if (!__atomic_load_n(&s->parked, __ATOMIC_ACQUIRE)) return 0;
    // Stock osThreadTerminate -> vTaskDelete removes this static task. Its
    // caller-owned TCB/stack may be reclaimed only after that call returns.
    if (DS_THREAD_TERMINATE(s->thread) != 0) return 0;
    s->thread = 0;
    if (s->clock_owned) {
        DS_REG(0xe0001000U)=s->old_dwt_ctrl;
        DS_REG(0xe000edfcU)=s->old_demcr;
        s->clock_owned=0;
    }
    *(ds_imports * volatile *)DS_IMPORT_SLOT = 0;
    return 1;
}
__attribute__((noinline)) static int ds_close_session(ds_session *s) {
    if (!ds_terminate(s)) { s->cancel = 1; return 0; }
    if (s->timer && (DS_TIMER_STOP(s->timer) != 0 || DS_TIMER_DELETE(s->timer) != 0)) return 0;
    s->timer = 0;
    customCfwContext *ctx = peekCustomCfwContext();
    if (ctx) ctx->decoder_speed_session = 0;
    ds_release_run(s);
    if (s->code_raw) FW_HEAP_FREE(DS_HEAP27,s->code_raw);
    if (s->clip) FW_FREE(s->clip);
    FW_FREE(s);
    return 1;
}
static void ds_lease_tick(void *argument) {
    customCfwContext *ctx = (customCfwContext *)argument;
    if (!ctx || !ctx->image_mutex || DS_MUTEX_TAKE(ctx->image_mutex, 0) != 0) return;
    ds_session *s = (ds_session *)ctx->decoder_speed_session;
    if (s && (int32_t)(FW_MS_TICK-s->deadline) >= 0) ds_close_session(s);
    DS_MUTEX_GIVE(ctx->image_mutex);
}
static void ds_cleanup(void) {
    ds_session *s = ds_active();
    if (s) { s->cancel = 1; s->deadline = FW_MS_TICK; ds_close_session(s); }
}

static int ds_pool_layout(ds_session *s) {
    uint8_t *p = s->hot;
    // Y and table offsets are unchanged between chroma modes and setups.
    for (uint32_t i=0; i<2; ++i) { ds_pool_add(&s->pool, 2, p, 61440); p += 61440; }
    const uint32_t sizes[8] = {3840,960,960,960,240,38400,3840,960};
    for (uint32_t i=0; i<8; ++i) { ds_pool_add(&s->pool, 5+i, p, sizes[i]); p += sizes[i]; }
    if (!s->skip)
        for (uint32_t plane=3; plane<=4; ++plane)
            for (uint32_t i=0; i<2; ++i) { ds_pool_add(&s->pool, plane, p, 15360); p += 15360; }
    ds_pool_add(&s->pool, 1, s->meta, 256);
    // Tiny vectors get their own slots; they cannot consume the RBSP slot.
    for (uint32_t i=0; i<16; ++i) ds_pool_add(&s->pool, 0, s->meta+256+i*32, 32);
    ds_pool_add(&s->pool, 0, s->meta+1024, 2048);
    return s->pool.count < DS_POOL_SLOTS;
}
static int ds_prepare(ds_session *s) {
    uint32_t hot_size=DS_LUMA_BYTES;
    uint32_t cached=DS_STATE_BYTES+8+DS_META_BYTES+DS_STACK_BYTES+32+hot_size+32;
    if (s->kind) cached+=sizeof(ds_profile);
    ds_record_heaps(s,16);
    if (!s->code_sealed || !s->code || !ds_room(20,cached+128,hot_size+32) || !ds_room(27,0,0)) return 0;
    s->hot_raw=FW_MALLOC(hot_size+32);
    s->state_raw=FW_MALLOC(DS_STATE_BYTES+8);
    s->meta=FW_MALLOC(DS_META_BYTES);
    s->stack_raw=FW_MALLOC(DS_STACK_BYTES+32);
    if (s->kind) s->profile=FW_MALLOC(sizeof(ds_profile));
    if (!s->hot_raw || !s->state_raw || !s->meta || !s->stack_raw || (s->kind && !s->profile)) goto failed;
    s->hot=ds_align(s->hot_raw,32); s->state=ds_align(s->state_raw,8); s->stack=ds_align(s->stack_raw,32);
    if (!ds_room(20,0,0) || !ds_room(27,0,0)) goto failed;
    if (s->profile) { bzero((uint8_t *)s->profile,sizeof(ds_profile)); s->profile->count=s->profile_count; }
    for (uint32_t i=0;i<DS_STACK_BYTES;++i) s->stack[i]=0xa5;
    bzero((uint8_t *)&s->pool,sizeof(s->pool));
    if (!ds_pool_layout(s)) goto failed;
    ds_record_heaps(s,22);
    s->words[28]=(uint32_t)(uintptr_t)s->clip;
    s->words[29]=(uint32_t)(uintptr_t)s->hot;
    s->words[30]=(uint32_t)(uintptr_t)s->state;
    s->words[31]=(uint32_t)(uintptr_t)s->meta;
    s->words[32]=(uint32_t)(uintptr_t)s->stack;
    s->words[33]=(uint32_t)(uintptr_t)s->code;
    s->words[34]=hot_size; s->words[7]=s->code_bytes; s->words[39]=sizeof(ds_session);
    s->words[110]=s->profile_count; s->words[113]=s->kind ? sizeof(ds_profile) : 0;
    s->words[114]=s->kind; s->words[115]=s->nonce;
    for (uint32_t i=0;i<8;++i) s->words[116+i]=ds_rd32(s->code_hash+4*i);
    for (uint32_t i=0;i<s->pool.count;++i) {
        s->words[40+i*2]=(uint32_t)(uintptr_t)s->pool.slots[i].data;
        s->words[41+i*2]=s->pool.slots[i].capacity;
    }
    return 1;
failed:
    ds_release_run(s); return 0;
}

static int ds_copy_code(ds_session *s) {
    uint32_t control, ipsr;
    ds_read_control(&control,&ipsr);
    s->words[8] = control; s->words[9] = DS_REG(0xe000ed94U);
    s->words[10] = DS_REG(0xe000ed14U); s->words[11] = DS_REG(0xe000ed7cU);
    // This donor uses the privileged default memory map for cached heap 27.
    // Require the audited MPU policy and enabled instruction/data caches.
    if (ipsr || (control & 1U) || s->words[9] != 7U || (s->words[10] & 0x30000U) != 0x30000U) return 0;
    uint32_t regions = (DS_REG(0xe000ed90U) >> 8) & 255U;
    uint32_t saved = DS_REG(0xe000ed98U);
    uint32_t address = (uint32_t)(uintptr_t)s->code;
    const uint32_t starts[7]={address,(uint32_t)(uintptr_t)s->clip,
        (uint32_t)(uintptr_t)s->state,(uint32_t)(uintptr_t)s->meta,
        (uint32_t)(uintptr_t)s->stack,(uint32_t)(uintptr_t)s->hot,(uint32_t)(uintptr_t)s->profile};
    const uint32_t sizes[7]={s->code_bytes,DS_CLIP_MAX_BYTES,DS_STATE_BYTES,
        DS_META_BYTES,DS_STACK_BYTES,DS_LUMA_BYTES,s->kind ? sizeof(ds_profile) : 0};
    for (uint32_t i=0; i<regions; ++i) {
        DS_REG(0xe000ed98U) = i;
        uint32_t base = DS_REG(0xe000ed9cU), limit = DS_REG(0xe000eda0U);
        if (!(limit&1U)) continue;
        for (uint32_t block=0; block<7; ++block) {
            if (!sizes[block]) continue;
            uint32_t end=starts[block]+sizes[block]-1;
            if (end<starts[block]) goto policy_failed;
            if (starts[block]>(limit|31U) || end<(base&~31U)) continue;
            // Every allocation uses cached privileged-default memory; an
            // explicit MPU region makes this donor layout unsupported.
            goto policy_failed;
        }
    }
    DS_REG(0xe000ed98U) = saved;
    uint32_t line = 4U << ((s->words[11] >> 16) & 15U);
    uint32_t iline = 4U << (s->words[11] & 15U);
    if (line > 64U || iline > 64U) return 0;
    ds_barrier(0);
    for (uint32_t p=address; p<address+s->code_bytes; p+=line) DS_REG(0xe000ef68U) = p;
    ds_barrier(0);
    for (uint32_t p=address; p<address+s->code_bytes; p+=iline) DS_REG(0xe000ef58U) = p; // ICIMVAU.
    ds_barrier(1);
    return 1;
policy_failed:
    DS_REG(0xe000ed98U)=saved;
    return 0;
}
static uint32_t ds_calibrate(void) {
    uint32_t guard = 3000000U, t = FW_MS_TICK;
    while (FW_MS_TICK == t && --guard) ds_nop();
    if (!guard) return 0;
    t = FW_MS_TICK;
    uint32_t c = DS_CYCLES;
    while ((uint32_t)(FW_MS_TICK-t) < 8 && --guard) ds_nop();
    uint32_t cycles = DS_CYCLES-c, ticks = FW_MS_TICK-t;
    if (!guard || ticks < 8 || ticks > 100) return 0;
    uint32_t rate = cycles/ticks;
    return rate >= 1000 && rate <= 500000 ? rate : 0;
}
static uint32_t ds_start_code(const uint8_t *p, uint32_t size, uint32_t at, uint32_t *prefix) {
    for (uint32_t i=at; i+3<=size; ++i)
        if (!p[i] && !p[i+1]) {
            if (p[i+2] == 1) { *prefix=3; return i; }
            if (i+4<=size && !p[i+2] && p[i+3] == 1) { *prefix=4; return i; }
        }
    *prefix=0; return size;
}
static int ds_pass(ds_session *s, uint32_t pass) {
#ifdef DS_HOST_TEST
    void *(*init)(void *,uint32_t,uint32_t) = ds_host_init;
    void (*destroy)(void *) = ds_host_destroy;
    int (*decode)(void *,const uint8_t *,uint32_t) = ds_host_decode;
    int (*frame)(const void *,ds_frame_info *) = ds_host_frame;
#else
    void *(*init)(void *,uint32_t,uint32_t) = (void *)(s->code+s->offsets[2]+1);
    void (*destroy)(void *) = (void *)(s->code+s->offsets[3]+1);
    int (*decode)(void *,const uint8_t *,uint32_t) = (void *)(s->code+s->offsets[4]+1);
    int (*frame)(const void *,ds_frame_info *) = (void *)(s->code+s->offsets[5]+1);
#endif
    void *state = init(s->state,DS_STATE_BYTES,s->skip);
    if (!state) return 10;
    uint32_t prefix, cursor=ds_start_code(s->clip,s->clip_bytes,0,&prefix), count=0;
    int error=0;
    while (prefix && cursor < s->clip_bytes) {
        uint32_t nal=cursor+prefix, next_prefix;
        uint32_t next=ds_start_code(s->clip,s->clip_bytes,nal,&next_prefix);
        if (nal>=next || next-nal>2048U) { error=12; break; }
        uint32_t type = s->clip[nal] & 31U;
        if (s->cancel || (int32_t)(FW_MS_TICK-s->run_deadline) >= 0) { error=11; break; }
        uint32_t rate = (type == 1 || type == 5) ? ds_calibrate() : 0;
        s->profile_recording=1;
        uint32_t tick0=FW_MS_TICK, c0=DS_CYCLES;
        int decoded=decode(state,s->clip+nal,next-nal);
        uint32_t cycles=DS_CYCLES-c0, ticks=FW_MS_TICK-tick0;
        s->profile_recording=0;
        if (decoded < 0) { error=13; break; }
        if (decoded == 1) {
            ds_frame_info info;
            if (count >= DS_FRAMES || frame(state,&info) || info.width != 320 || info.height != 192
                || info.stride != 320 || info.count != count+1 || !info.y) { error=14; break; }
            uint32_t after=ds_calibrate();
            // Clock anomalies are evidence, not decoder failures. The PC
            // flags/excludes their timing while still checking every Y hash.
            uint32_t hash=~ds_crc(info.y,61440,~0U);
            uint32_t row=DS_HEADER_WORDS+(pass*DS_FRAMES+count)*DS_FRAME_WORDS;
            s->words[row]=cycles; s->words[row+1]=hash;
            s->words[row+2]=ticks; s->words[row+3]=rate; s->words[row+4]=after;
            s->used_words=row+DS_FRAME_WORDS;
            ++count;
            DS_DELAY(1); // Normal scheduler, interrupts and watchdog remain live.
        }
        if (s->cancel || (int32_t)(FW_MS_TICK-s->run_deadline) >= 0) { error=11; break; }
        cursor=next; prefix=next_prefix;
    }
    destroy(state);
    if (!ds_pool_reset(&s->pool)) return 16;
    if (!error && count != DS_FRAMES) return 17;
    return error;
}
static void ds_worker(void *argument) {
    ds_session *s = argument;
    while (!__atomic_load_n(&s->start,__ATOMIC_ACQUIRE)) {
        if (s->cancel || (int32_t)(FW_MS_TICK-s->run_deadline) >= 0) {
            s->phase=DS_CANCELLED; ds_park(s);
        }
        DS_DELAY(1);
    }
    if (!ds_copy_code(s)) { s->error=1; goto complete; }
    uint32_t (*selftest)(uint32_t) = (void *)(s->code+s->offsets[1]+1);
    uint32_t (*state_size)(void) = (void *)(s->code+s->offsets[0]+1);
    if (selftest(0x12345678U) != ((0x12345678U ^ 0xd35c2301U)+17U)
        || state_size() > DS_STATE_BYTES) { s->error=2; goto complete; }
    s->words[35]=state_size();
    uint32_t old_demcr=DS_REG(0xe000edfcU), old_ctrl=DS_REG(0xe0001000U);
    s->old_demcr=old_demcr; s->old_dwt_ctrl=old_ctrl; s->clock_owned=1;
    DS_REG(0xe0001fb0U)=0xc5acce55U;
    DS_REG(0xe000edfcU)=old_demcr | 0x1000000U;
    DS_REG(0xe0001000U)=old_ctrl | 1U;
    *(ds_imports * volatile *)DS_IMPORT_SLOT=&s->imports;
    for (uint32_t pass=0; pass<(s->kind ? 2U : 7U); ++pass) {
        s->error=ds_pass(s,pass);
        if (s->error) break;
    }
    DS_REG(0xe0001000U)=old_ctrl; DS_REG(0xe000edfcU)=old_demcr;
    s->clock_owned=0;
    s->words[36]=s->pool.highwater;
    s->words[37]=s->kind ? 1U : 2U;
    if (s->profile) {
        s->words[111]=s->profile->flags | (s->profile->depth ? 16U : 0U);
        s->words[112]=s->profile->highwater;
    }
    uint32_t untouched=0;
    while (untouched<DS_STACK_BYTES && s->stack[untouched]==0xa5) ++untouched;
    s->words[38]=DS_STACK_BYTES-untouched;
    if (untouched<1024) s->error=18;
complete:
    s->words[2]=s->error;
    s->words[3]=s->used_words;
    __atomic_store_n(&s->phase,s->cancel ? DS_CANCELLED : s->error ? DS_FAILED : DS_DONE,__ATOMIC_RELEASE);
    // One completion notification AFTER all timing; READ commands then pull
    // bounded result words. No BLE send occurs inside any decode bracket.
    ds_reply(s->session,0xffffU,s->phase);
    ds_park(s);
}

/* Ownership is explicit, not an authentication claim: stock login is public.
 * The transport rejects bridged/both-lens mode-31 commands before this parser. */
static int ds_write_blob(uint8_t *target, uint32_t capacity, uint32_t *uploaded,
                         const uint8_t *p, uint32_t n) {
    if (n<=4 || n>2052) return -1;
    uint32_t at=ds_rd32(p); p+=4; n-=4;
    if (at>capacity || n>capacity-at) return -1;
    if (at<*uploaded) {
        if (n>*uploaded-at) return -1;
        for (uint32_t i=0;i<n;++i) if (target[at+i]!=p[i]) return -1;
        return 0;
    }
    if (at!=*uploaded) return -1;
    memcpy(target+at,p,n); *uploaded+=n; return 0;
}
static int ds_control(const uint8_t *data, uint32_t size) {
    if (size<11 || data[0]!=31 || data[1]!='D' || data[2]!='S' || data[3]!=3) return -1;
    uint32_t op=data[4], token=ds_rd16(data+5), nonce=ds_rd32(data+7);
    const uint8_t *p=data+11; uint32_t n=size-11;
    customCfwContext *ctx=getCustomCfwContext();
    ds_session *s=ds_active();
    if (!token || !nonce) return -1;
    if (op==DS_HELLO && !n) return ds_reply(token,0xffffU,DS_MAGIC);
    if (op==DS_HEAPS && n==2 && !s) {
        uint32_t index=ds_rd16(p);
        if (index>=6) return -1;
        const uint32_t heaps[3]={20,13,27}; cfw_heap_stats stats=ds_stats(heaps[index/2]);
        return ds_reply(token,index,index&1 ? stats.max_alloc : stats.free_bytes);
    }
    if (op==DS_BEGIN && n==88) {
        uint32_t bytes=ds_rd32(p),crc=ds_rd32(p+4),code_bytes=ds_rd32(p+12);
        uint32_t lo=ds_rd32(p+16),hi=ds_rd32(p+20),flags=ds_rd32(p+24),count=ds_rd32(p+28);
        if (ds_clip_index(bytes,crc)<0 || bytes>DS_CLIP_MAX_BYTES || ds_rd16(p+8)!=320 || ds_rd16(p+10)!=192
            || code_bytes<16 || code_bytes>DS_CAPSULE_MAX_BYTES || lo>=hi || hi>code_bytes
            || (flags>>16)!=1 || (flags&0xffff)>2 || count>DS_PROFILE_FUNCTIONS
            || ((flags&0xffff) ? !count : count!=0)) return -1;
        for (uint32_t i=0;i<6;++i) {
            uint32_t off=ds_rd32(p+32+i*4);
            if ((off&1U) || off<lo || off>=hi) return -1;
            for (uint32_t j=0;j<i;++j) if (off==ds_rd32(p+32+j*4)) return -1;
        }
        if (s) return -1; /* Single owner; an OPEN retry never replaces it. */
        if (!ds_room(20,sizeof(ds_session)+DS_CLIP_MAX_BYTES+128,DS_CLIP_MAX_BYTES)
            || !ds_room(27,code_bytes+128,code_bytes+128)) return -1;
        s=FW_MALLOC(sizeof(ds_session)); if (!s) return -1;
        bzero((uint8_t *)s,sizeof(*s));
        s->session=token; s->nonce=nonce; s->phase=DS_UPLOAD; s->skip=1; s->setup=4;
        s->clip_bytes=bytes; s->clip_crc=crc; s->code_bytes=code_bytes;
        s->text_lo=lo; s->text_hi=hi; s->kind=flags&0xffff; s->profile_count=count;
        for (uint32_t i=0;i<6;++i) s->offsets[i]=ds_rd32(p+32+i*4);
        memcpy(s->code_hash,p+56,32);
        s->imports=(ds_imports){ds_allocate,ds_deallocate,ds_preallocate,ds_fatal,ds_profile_push,ds_profile_pop};
        s->used_words=DS_HEADER_WORDS;
        s->clip=FW_MALLOC(DS_CLIP_MAX_BYTES);
        s->code_raw=FW_HEAP_MALLOC(DS_HEAP27,code_bytes+128);
        if (s->code_raw) s->code=ds_align(s->code_raw,64);
        s->deadline=FW_MS_TICK+120000U;
        s->timer=DS_TIMER_NEW(ds_lease_tick,1,ctx,0);
        if (!s->clip || !s->code_raw || !s->timer || !ds_room(20,0,0) || !ds_room(27,0,0)
            || DS_TIMER_START(s->timer,1000)!=0) {
            if (s->timer) DS_TIMER_DELETE(s->timer);
            if (s->clip) FW_FREE(s->clip);
            if (s->code_raw) FW_HEAP_FREE(DS_HEAP27,s->code_raw);
            FW_FREE(s); return -1;
        }
        ctx->decoder_speed_session=s; return 0;
    }
    if (!s || s->session!=token || s->nonce!=nonce) return -1;
    s->deadline=FW_MS_TICK+120000U;
    if (op==DS_WRITE && s->phase==DS_UPLOAD && !s->clip_sealed)
        return ds_write_blob(s->clip,s->clip_bytes,&s->uploaded,p,n);
    if (op==DS_CAP_WRITE && s->phase==DS_UPLOAD && !s->code_sealed)
        return ds_write_blob((uint8_t *)s->code,s->code_bytes,&s->code_uploaded,p,n);
    if ((op==DS_SEAL || op==DS_CAP_SEAL) && !n && (s->phase==DS_UPLOAD || s->phase==DS_SEALED)) {
        if (op==DS_SEAL) {
            if (s->uploaded!=s->clip_bytes || ~ds_crc(s->clip,s->clip_bytes,~0U)!=s->clip_crc) return -1;
            s->clip_sealed=1;
        } else {
            if (s->code_uploaded!=s->code_bytes) return -1;
            uint8_t hash[32]; ds_sha256(s->code,s->code_bytes,hash);
            for (uint32_t i=0;i<32;++i) if (hash[i]!=s->code_hash[i]) return -1;
            s->code_sealed=1;
        }
        if (s->code_sealed && s->clip_sealed) s->phase=DS_SEALED;
        return 0;
    }
    if (op==DS_RUN && n==4) {
        if (p[0]!=4 || p[1]!=1 || p[2]!=5 || p[3]) return -1;
        if (s->phase!=DS_SEALED || !s->code_sealed || !s->clip_sealed) return -1;
        s->words[0]=DS_MAGIC; s->words[1]=4 | 1U<<8;
        s->words[4]=DS_FRAMES; s->words[5]=s->kind ? 1U : 5U; s->words[6]=s->clip_crc;
        s->words[12]=320; s->words[13]=192; s->words[14]=s->clip_bytes; s->words[15]=DS_FRAME_WORDS;
        if (!ds_prepare(s)) return -1;
        ds_thread_attr attr={"decoder-speed",0,s->tcb,sizeof(s->tcb),s->stack,DS_STACK_BYTES,8,0,0};
        s->run_deadline=FW_MS_TICK+60000U; s->phase=DS_RUNNING;
        s->thread=DS_THREAD_NEW(ds_worker,s,&attr);
        if (!s->thread) { s->phase=DS_SEALED; ds_release_run(s); return -1; }
        return 0;
    }
    if (op==DS_READ && n==2) {
        uint32_t index=ds_rd16(p);
        if (index==0xffffU) return ds_reply(token,index,s->phase);
        if ((s->phase!=DS_DONE && s->phase!=DS_FAILED && s->phase!=DS_CANCELLED) || index>=s->used_words) return -1;
        return ds_reply(token,index,s->words[index]);
    }
    if (op==DS_PROFILE_READ && n==2) {
        uint32_t index=ds_rd16(p);
        if (s->phase!=DS_DONE || !s->profile || index>=s->profile_count*5) return -1;
        ds_profile_row *r=&s->profile->rows[index/5];
        uint32_t word=index%5, value=word==0 ? r->calls : word==1 ? (uint32_t)r->inclusive
            : word==2 ? (uint32_t)(r->inclusive>>32) : word==3 ? (uint32_t)r->exclusive : (uint32_t)(r->exclusive>>32);
        return ds_reply(token,index,value);
    }
    if (op==DS_ABORT && !n) { s->cancel=1; return 0; }
    if (op==DS_CLOSE && !n) { s->cancel=1; return ds_close_session(s) ? 0 : -1; }
    return -1;
}
static void ds_after_ack(const uint8_t *data, uint32_t size) {
    if (size!=15 || data[0]!=31 || data[1]!='D' || data[2]!='S' || data[3]!=3 || data[4]!=DS_RUN) return;
    customCfwContext *ctx=peekCustomCfwContext();
    if (!ctx || DS_MUTEX_TAKE(ctx->image_mutex,10)!=0) return;
    ds_session *s=ds_active();
    if (s && s->session==ds_rd16(data+5) && s->nonce==ds_rd32(data+7) && s->phase==DS_RUNNING)
        __atomic_store_n(&s->start,1,__ATOMIC_RELEASE);
    DS_MUTEX_GIVE(ctx->image_mutex);
}
#pragma clang section text=""
