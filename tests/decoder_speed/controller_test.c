/* Exercise the actual controller without an emulator, Bluetooth or glasses. */
#include <assert.h>
#define DS_HOST_TEST
#include "host_platform.h"
#include "../../patches/decoder_speed/controller.c"

static uint8_t command[2059];
static uint32_t header(uint32_t op,uint32_t n) {
    memset(command,0,sizeof(command));
    command[0]=31; command[1]='D'; command[2]='S'; command[3]=2;
    command[4]=(uint8_t)op; command[5]=1; return n;
}
static void put32(uint32_t at,uint32_t v) {
    for(uint32_t i=0;i<4;++i) command[at+i]=(uint8_t)(v>>(i*8));
}
static int begin(void) {
    header(DS_BEGIN,19); put32(7,DS_CLIP_BYTES); put32(11,DS_CLIP_CRC);
    command[15]=64; command[16]=1; command[17]=192;
    return ds_control(command,19);
}
static int run(uint32_t ram,uint32_t skip,uint32_t uncached) {
    header(DS_RUN,11); command[7]=ram; command[8]=skip;
    command[9]=5; command[10]=uncached; return ds_control(command,11);
}
static void close_session(void) {
    header(DS_CLOSE,7); assert(ds_control(command,7)==0);
    assert(!context.decoder_speed_session && allocations==releases);
}
static void reset(void) {
    assert(!context.decoder_speed_session && allocations==releases);
    fail_at=timer_fail=thread_new_fail=mutex_busy=0; tick=0;
}
static void protocol_and_default(void) {
    header(DS_HELLO,7); command[3]=1;
    assert(ds_control(command,7)==-1 && !allocations); /* Old row layout is incompatible. */
    header(DS_HELLO,7); assert(ds_control(command,7)==0);
    assert(!allocations && !context.decoder_speed_session);
    ds_cleanup(); assert(!allocations);
    header(DS_BEGIN,19); assert(ds_control(command,19)==-1 && !allocations);
    assert(begin()==0); uint32_t live=allocations-releases;
    assert(begin()==0 && allocations-releases==live);
    command[5]=2; assert(ds_control(command,19)==-1);
    header(DS_WRITE,12); put32(7,1); assert(ds_control(command,12)==-1);
    put32(7,0); command[11]=123; assert(ds_control(command,12)==0);
    assert(ds_control(command,12)==0 && ds_active()->uploaded==1);
    command[11]=122; assert(ds_control(command,12)==-1);
    put32(7,DS_CLIP_BYTES); assert(ds_control(command,12)==-1);
    header(DS_SEAL,7); assert(ds_control(command,7)==-1);
    ds_active()->uploaded=DS_CLIP_BYTES; memset(ds_active()->clip,0,DS_CLIP_BYTES);
    assert(ds_control(command,7)==-1); /* A complete upload with a bad CRC. */
    header(DS_READ,9); assert(ds_control(command,9)==-1);
    close_session(); reset();
}
static void failed_reservations_release_only_owned_blocks(void) {
    for(uint32_t failure=1;failure<=2;++failure) {
        fail_at=allocation_attempts+failure;
        assert(begin()==-1); assert(!context.decoder_speed_session && allocations==releases); reset();
    }
    timer_fail=1; assert(begin()==-1 && allocations==releases); reset();
    for(uint32_t failure=1;failure<=5;++failure) {
        assert(begin()==0); ds_active()->phase=DS_SEALED;
        fail_at=allocation_attempts+failure;
        assert(run(1,1,0)==-1 && ds_active()->phase==DS_SEALED);
        assert(!ds_active()->hot_raw && !ds_active()->code_raw);
        fail_at=0; close_session(); reset();
    }
    assert(begin()==0); ds_active()->phase=DS_SEALED;
    thread_new_fail=1; assert(run(0,0,0)==-1 && !ds_active()->hot_raw);
    close_session(); reset();
}
static void worker_gating_and_lifetime(void) {
    assert(begin()==0); ds_session *s=ds_active(); s->phase=DS_SEALED;
    assert(run(1,0,1)==-1); /* Uncached data is MRAM-only. */
    assert(run(1,0,0)==0 && !s->start && s->code_raw);
    assert(s->words[0]==0x44530203U && s->words[15]==DS_FRAME_WORDS);
    assert(run(1,0,0)==0 && !s->start);
    mutex_busy=1; ds_after_ack(command,11); assert(!s->start);
    mutex_busy=0; ds_after_ack(command,11); assert(s->start);
    uint32_t live=allocations-releases, deletes=terminated;
    header(DS_CLOSE,7); assert(ds_control(command,7)==-1);
    assert(s->cancel && allocations-releases==live && terminated==deletes);
    s->parked=1; s->clock_owned=1; s->old_demcr=123; s->old_dwt_ctrl=456;
    timer_fail=1; assert(ds_control(command,7)==-1 && !s->thread && s->code_raw);
    assert(terminated==deletes+1 && regs[5]==123 && regs[6]==456);
    timer_fail=0; close_session(); reset();
    assert(begin()==0); s=ds_active(); tick=s->deadline;
    ds_lease_tick(&context); assert(!ds_active() && allocations==releases); reset();
}
static void memory_policy_fails_closed(void) {
    ds_session s={0}; s.code=(void *)0x007cb000U;
    s.clip=(void *)0x20204000U; s.state=(void *)0x20210000U;
    s.meta=(void *)0x20212000U; s.stack=(void *)0x20214000U; s.hot=(void *)0x20220000U;
    regs[1]=7; regs[3]=0x30000; regs[2]=9;
    regs[0]=0; assert(ds_copy_code(&s));
    regs[8]=1; assert(!ds_copy_code(&s)); regs[8]=0;
    regs[9]=1; assert(!ds_copy_code(&s)); regs[9]=0;
    regs[3]=0; assert(!ds_copy_code(&s)); regs[3]=0x30000;
    regs[0]=1U<<8; mpu_base=0x20200001U; mpu_limit=0x2027ffe1U; mpu_mair=0x44;
    assert(!ds_copy_code(&s) && regs[2]==9); /* Cached data covered by uncached MPU. */
    s.hot=(void *)0x20140000U; s.uncached=1;
    mpu_base=0x201350a1U; mpu_limit=0x20202181U;
    assert(ds_copy_code(&s) && regs[2]==9);
    mpu_mair=0xff; assert(!ds_copy_code(&s));
    mpu_mair=0x44; mpu_base&=~1U; assert(!ds_copy_code(&s));
    regs[0]=0; assert(!ds_copy_code(&s)); /* No uncached region. */
}
static uint8_t fake_y[61440];
static uint32_t fake_frames, destroyed, decode_error_at, bad_format, short_clip;
static uint32_t invalid_clock, cancel_at, deadline_at;
static void *ds_host_init(void *memory,uint32_t size,uint32_t skip) {
    (void)size; (void)skip; fake_frames=0; calibration_rate=250000; return memory;
}
static void ds_host_destroy(void *state) { (void)state; ++destroyed; }
static int ds_host_decode(void *state,const uint8_t *nal,uint32_t size) {
    (void)state; (void)nal; (void)size;
    ++fake_frames; tick+=3; regs[7]+=750000;
    // More than 4% drift, then unavailable calibration: neither stops decode.
    calibration_rate=invalid_clock ? 0 : fake_frames&1 ? 230000 : 250000;
    if (fake_frames==cancel_at) ds_active()->cancel=1;
    if (fake_frames==deadline_at) tick=ds_active()->run_deadline;
    if (fake_frames==decode_error_at) return -1;
    return short_clip && fake_frames==DS_FRAMES ? 0 : 1;
}
static int ds_host_frame(const void *state,ds_frame_info *info) {
    (void)state;
    *info=(ds_frame_info){fake_y,bad_format ? 640U : 320U,192,320,fake_frames};
    return 0;
}
static void timing_anomalies_do_not_abort_but_real_failures_do(void) {
    assert(begin()==0); ds_session *s=ds_active();
    memset(s->clip,0,DS_CLIP_BYTES);
    for(uint32_t i=0;i<DS_FRAMES;++i) {
        s->clip[i*5+2]=1; s->clip[i*5+3]=1; s->clip[i*5+4]=0xaa;
    }
    s->state=fake_y; s->run_deadline=tick+60000;
    uint32_t expected_hash=~ds_crc(fake_y,sizeof(fake_y),~0U);
    for(uint32_t pass=0;pass<7;++pass) {
        invalid_clock=pass&1;
        assert(ds_pass(s,pass)==0);
        assert(s->used_words==DS_HEADER_WORDS+(pass+1)*DS_FRAMES*DS_FRAME_WORDS);
        uint32_t at=DS_HEADER_WORDS+pass*DS_FRAMES*DS_FRAME_WORDS;
        assert(s->words[at]==750000 && s->words[at+1]==expected_hash && s->words[at+2]==3);
        assert(s->words[at+3]==250000 && s->words[at+4]==(invalid_clock ? 0U : 230000U));
    }
    assert(s->used_words==DS_WORDS && destroyed==7);
    invalid_clock=0; decode_error_at=3; assert(ds_pass(s,0)==13); decode_error_at=0;
    bad_format=1; assert(ds_pass(s,0)==14); bad_format=0;
    short_clip=1; assert(ds_pass(s,0)==17); short_clip=0;
    deadline_at=3; assert(ds_pass(s,0)==11); deadline_at=0;
    s->run_deadline=tick+60000;
    cancel_at=3; assert(ds_pass(s,0)==11); cancel_at=0;
    close_session(); reset();
}
int main(void) {
    protocol_and_default(); failed_reservations_release_only_owned_blocks();
    worker_gating_and_lifetime(); memory_policy_fails_closed();
    timing_anomalies_do_not_abort_but_real_failures_do();
    return 0;
}
