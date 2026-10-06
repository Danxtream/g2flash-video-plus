/* Exercise upload integrity, ownership and lifetime without device access. */
#include <assert.h>
#include <stdio.h>
#define DS_HOST_TEST
#include "host_platform.h"
#include "../../patches/decoder_speed/controller.c"

static uint8_t command[2063], capsule[128];
static uint32_t header(uint32_t op,uint32_t n) {
    memset(command,0,sizeof(command));
    command[0]=31; command[1]='D'; command[2]='S'; command[3]=3;
    command[4]=(uint8_t)op; command[5]=1; command[7]=123; return n;
}
static void put32(uint32_t at,uint32_t v) {
    for(uint32_t i=0;i<4;++i) command[at+i]=(uint8_t)(v>>(i*8));
}
static int begin(void) {
    header(DS_BEGIN,99); put32(11,DS_CLIP_BYTES); put32(15,DS_CLIP_CRC);
    command[19]=64; command[20]=1; command[21]=192;
    put32(23,sizeof(capsule)); put32(27,0); put32(31,sizeof(capsule)); put32(35,1U<<16);
    for(uint32_t i=0;i<6;++i) put32(43+i*4,i*2);
    ds_sha256(capsule,sizeof(capsule),command+67);
    return ds_control(command,99);
}
static int run(void) {
    header(DS_RUN,15); command[11]=4; command[12]=1; command[13]=5;
    return ds_control(command,15);
}
static void close_session(void) {
    header(DS_CLOSE,11); assert(ds_control(command,11)==0);
    assert(!context.decoder_speed_session && allocations==releases);
}
static void reset(void) {
    assert(!context.decoder_speed_session && allocations==releases);
    fail_at=timer_fail=thread_new_fail=mutex_busy=0; tick=0;
    heap27_free=172620; heap27_max=172032;
}
static void ready(void) {
    /* Tests of RUN failure/lifetime use a sealed fixture without executing it. */
    assert(begin()==0); ds_session *s=ds_active();
    memcpy((void *)s->code,capsule,sizeof(capsule));
    s->code_uploaded=sizeof(capsule); header(DS_CAP_SEAL,11); assert(ds_control(command,11)==0);
    s->clip_sealed=1; s->phase=DS_SEALED;
}
static void inert_scope_and_integrity(void) {
    header(DS_HELLO,11); command[3]=2; assert(ds_control(command,11)==-1 && !allocations);
    header(DS_HELLO,11); assert(ds_control(command,11)==0 && !allocations);
    ds_cleanup(); assert(!allocations);
    uint8_t body=31;
    assert(ds_direct_message(&body,1,1,1,1));
    assert(!ds_direct_message(&body,1,1,2,1));
    assert(!ds_direct_message(&body,1,1,1,3));
    body=1; assert(ds_direct_message(&body,1,1,2,3));
    assert(begin()==0); assert(begin()==-1); /* Cannot replace an owner. */
    header(DS_CAP_WRITE,16); put32(11,0); command[15]=42;
    command[7]^=1; assert(ds_control(command,16)==-1); command[7]^=1;
    assert(ds_control(command,16)==0 && ds_active()->code_uploaded==1);
    assert(ds_control(command,16)==0); command[15]^=1; assert(ds_control(command,16)==-1);
    put32(11,2); assert(ds_control(command,16)==-1);
    put32(11,0xffffffffU); assert(ds_control(command,16)==-1);
    header(DS_CAP_SEAL,11); assert(ds_control(command,11)==-1);
    ds_active()->code_uploaded=sizeof(capsule); memset((void *)ds_active()->code,7,sizeof(capsule));
    assert(ds_control(command,11)==-1 && !ds_active()->code_sealed);
    memcpy((void *)ds_active()->code,capsule,sizeof(capsule));
    assert(ds_control(command,11)==0 && ds_active()->code_sealed);
    header(DS_CAP_WRITE,16); put32(11,0); assert(ds_control(command,16)==-1);
    assert(run()==-1); /* Clip has not been sealed. */
    header(DS_SEAL,11); assert(ds_control(command,11)==-1);
    close_session(); reset();
    assert(begin()==0); close_session(); reset();
    /* Validate metadata before allocating, including overflow and aliases. */
    uint8_t valid[99]; assert(begin()==0); memcpy(valid,command,99); close_session(); reset();
    const uint32_t positions[6]={23,27,43,47,35,7};
    const uint32_t values[6]={156109,129,1,0,2U<<16,0};
    for(uint32_t field=0;field<6;++field) {
        memcpy(command,valid,99); put32(positions[field],values[field]);
        uint32_t before=allocations;
        assert(ds_control(command,99)==-1 && allocations==before && !ds_active());
    }
}
static void reservations_worker_and_lifetime(void) {
    for(uint32_t failure=1;failure<=3;++failure) {
        fail_at=allocation_attempts+failure; assert(begin()==-1 && allocations==releases); reset();
    }
    timer_fail=1; assert(begin()==-1 && allocations==releases); reset();
    for(uint32_t failure=1;failure<=4;++failure) {
        ready(); fail_at=allocation_attempts+failure;
        assert(run()==-1 && !ds_active()->hot_raw && ds_active()->code_raw);
        fail_at=0; close_session(); reset();
    }
    ready(); thread_new_fail=1; assert(run()==-1 && !ds_active()->hot_raw && ds_active()->code_raw);
    close_session(); reset();
    ready(); ds_session *s=ds_active(); assert(run()==0 && !s->start);
    assert(run()==-1); /* A RUN is never replayed, even while executing. */
    header(DS_RUN,15); command[11]=4; command[12]=1; command[13]=5;
    command[7]^=1; ds_after_ack(command,15); assert(!s->start); command[7]^=1;
    mutex_busy=1; ds_after_ack(command,15); assert(!s->start);
    mutex_busy=0; ds_after_ack(command,15); assert(s->start);
    uint32_t live=allocations-releases;
    header(DS_CLOSE,11); assert(ds_control(command,11)==-1 && s->cancel && allocations-releases==live);
    s->parked=1; s->clock_owned=1; s->old_demcr=123; s->old_dwt_ctrl=456;
    timer_fail=1; assert(ds_control(command,11)==-1 && !s->thread && s->code_raw);
    assert(regs[5]==123 && regs[6]==456); timer_fail=0; close_session(); reset();
    assert(begin()==0); tick=ds_active()->deadline; ds_lease_tick(&context);
    assert(!ds_active() && allocations==releases); reset();
    heap27_free=sizeof(capsule)+128+16384-1; assert(begin()==-1); reset();
    heap27_max=128; assert(begin()==-1); reset();
}
static void memory_policy_fails_closed(void) {
    ds_session s={0}; s.code=(void *)0x20275000U; s.code_bytes=128; s.clip_bytes=DS_CLIP_BYTES;
    s.clip=(void *)0x20204000U; s.state=(void *)0x20210000U; s.skip=1;
    s.meta=(void *)0x20212000U; s.stack=(void *)0x20214000U; s.hot=(void *)0x20220000U;
    regs[1]=7; regs[3]=0x30000; regs[2]=9; regs[0]=0; assert(ds_copy_code(&s));
    regs[8]=1; assert(!ds_copy_code(&s)); regs[8]=0;
    regs[9]=1; assert(!ds_copy_code(&s)); regs[9]=0;
    regs[3]=0; assert(!ds_copy_code(&s)); regs[3]=0x30000;
    regs[0]=1U<<8; mpu_base=0x20200001U; mpu_limit=0x2027ffe1U;
    assert(!ds_copy_code(&s) && regs[2]==9); regs[0]=0;
}
static uint8_t fake_y[61440];
static uint32_t fake_frames, destroyed, decode_error_at, bad_format, short_clip;
static uint32_t invalid_clock, cancel_at, deadline_at;
static void *ds_host_init(void *memory,uint32_t size,uint32_t skip) {
    (void)size; (void)skip; fake_frames=0; calibration_rate=250000;
    ds_profile_push(0); ds_profile_pop(0); return memory;
}
static void ds_host_destroy(void *state) {
    (void)state; ds_profile_push(0); ds_profile_pop(0); ++destroyed;
}
static int ds_host_decode(void *state,const uint8_t *nal,uint32_t size) {
    (void)state; (void)nal; (void)size;
    ds_profile_push(0);
    ++fake_frames; tick+=3; regs[7]+=750000;
    ds_profile_pop(0);
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
    s->clip_bytes=DS_FRAMES*5;
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
    s->cancel=0; s->run_deadline=tick+60000;
    ds_profile profile={0}; profile.count=1; s->kind=1; s->profile=&profile;
    assert(ds_pass(s,0)==0 && ds_pass(s,1)==0);
    assert(!profile.flags && !profile.depth && profile.rows[0].calls==64);
    assert(profile.rows[0].inclusive==64ULL*750000);
    assert(!s->profile_recording); s->profile=0;
    close_session(); reset();
}

static void verified_clip_upload(const char *path) {
    FILE *file=fopen(path,"rb"); assert(file);
    fseek(file,0,SEEK_END); long length=ftell(file); rewind(file);
    assert(length>0 && length<=DS_CLIP_MAX_BYTES);
    uint8_t *clip=malloc((size_t)length); assert(clip);
    assert(fread(clip,1,(size_t)length,file)==(size_t)length); fclose(file);
    uint32_t crc=~ds_crc(clip,(uint32_t)length,~0U);
    assert(ds_clip_index((uint32_t)length,crc)>=0);
    ready(); ds_session *s=ds_active(); s->phase=DS_UPLOAD; s->clip_sealed=0;
    s->clip_bytes=(uint32_t)length; s->clip_crc=crc;
    for(uint32_t offset=0;offset<(uint32_t)length;) {
        uint32_t bytes=(uint32_t)length-offset; if(bytes>2048) bytes=2048;
        header(DS_WRITE,15+bytes); put32(11,offset); memcpy(command+15,clip+offset,bytes);
        assert(ds_control(command,15+bytes)==0); offset+=bytes;
    }
    header(DS_SEAL,11); assert(ds_control(command,11)==0);
    assert(run()==0 && s->words[6]==crc && s->words[14]==(uint32_t)length && s->skip==1);
    s->parked=1; close_session(); reset(); free(clip);
}
int main(int argc,char **argv) {
    for(uint32_t i=0;i<sizeof(capsule);++i) capsule[i]=(uint8_t)(i*17);
    reset(); inert_scope_and_integrity(); reservations_worker_and_lifetime(); memory_policy_fails_closed();
    timing_anomalies_do_not_abort_but_real_failures_do();
    for(int i=1;i<argc;++i) verified_clip_upload(argv[i]);
    puts("PASS: C nonce/seal/direct-lens/allocation/park/cache/timing and supplied clips");
    return 0;
}
