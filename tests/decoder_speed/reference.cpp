// Host proof of firmware A's capsule and reserved-pool path. Clips are arguments.
#include "../../patches/decoder_speed/pool.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern ds_imports* ds_host_imports;
static ds_pool pool;
alignas(32) static uint8_t hot[234480], metadata[3072], state[4096];
static void* allocate(uint32_t n) { return ds_pool_alloc(&pool,n); }
static void release(void* p) { if (!ds_pool_free(&pool,p)) std::exit(7); }
static int preflight(const ds_request* r,uint32_t n,ds_failure* f) {
    return ds_pool_preflight(&pool,r,n,f);
}
static void fail(uint32_t reason) { std::fprintf(stderr,"capsule allocation failure %u\n",reason); std::exit(8); }
static uint32_t crc(const uint8_t* p,uint32_t n) {
    uint32_t c=~0U;
    while(n--) { c ^= *p++; for(uint32_t i=0;i<8;++i) c=(c>>1)^(0xedb88320U & (0U-(c&1U))); }
    return ~c;
}
static uint32_t start(const uint8_t* p,uint32_t n,uint32_t at,uint32_t& prefix) {
    for(uint32_t i=at;i+3<=n;++i) if(!p[i] && !p[i+1]) {
        if(p[i+2]==1) { prefix=3; return i; }
        if(i+4<=n && !p[i+2] && p[i+3]==1) { prefix=4; return i; }
    }
    prefix=0; return n;
}
int main(int argc,char** argv) {
    if(argc!=3) { std::fprintf(stderr,"usage: reference clip.h264 skip(0|1)\n"); return 1; }
    bool skip=std::atoi(argv[2])!=0;
    FILE* file=std::fopen(argv[1],"rb"); if(!file) return 2;
    std::fseek(file,0,SEEK_END); long length=std::ftell(file); std::rewind(file);
    if(length!=23921) return 3;
    auto* clip=static_cast<uint8_t*>(std::malloc(length));
    if(!clip || std::fread(clip,1,length,file)!=static_cast<size_t>(length)) return 3;
    std::fclose(file);
    if(crc(clip,length)!=0xc81c1bdcU) return 4;
    uint8_t* p=hot;
    for(uint32_t i=0;i<2;++i) { ds_pool_add(&pool,2,p,61440); p+=61440; }
    const uint32_t sizes[8]={3840,960,960,960,240,38400,3840,960};
    for(uint32_t i=0;i<8;++i) { ds_pool_add(&pool,5+i,p,sizes[i]); p+=sizes[i]; }
    if(!skip) for(uint32_t tag=3;tag<=4;++tag) for(uint32_t i=0;i<2;++i) { ds_pool_add(&pool,tag,p,15360); p+=15360; }
    ds_pool_add(&pool,1,metadata,256);
    for(uint32_t i=0;i<16;++i) ds_pool_add(&pool,0,metadata+256+i*32,32);
    ds_pool_add(&pool,0,metadata+1024,2048);
    ds_imports imports={allocate,release,preflight,fail}; ds_host_imports=&imports;
    for(uint32_t pass=0;pass<7;++pass) {
        void* handle=ds_init(state,sizeof(state),skip);
        if(!handle) return 5;
        uint32_t prefix, cursor=start(clip,length,0,prefix), frames=0;
        while(prefix && cursor<static_cast<uint32_t>(length)) {
            uint32_t nal=cursor+prefix, next_prefix;
            uint32_t next=start(clip,length,nal,next_prefix);
            int result=ds_decode(handle,clip+nal,next-nal);
            if(result<0) return 6;
            if(result==1) {
                ds_frame_info info;
                if(ds_frame(handle,&info) || info.width!=320 || info.height!=192 || info.stride!=320 || info.count!=frames+1) return 6;
                std::printf("{\"pass\":%u,\"frame\":%u,\"hash\":%u}\n",pass,frames,crc(info.y,61440));
                ++frames;
            }
            cursor=next; prefix=next_prefix;
        }
        ds_destroy(handle);
        if(frames!=32 || !ds_pool_reset(&pool)) return 9;
    }
    std::fprintf(stderr,"PASS: 7 passes, 224 frames; pool highwater=%u; state=%u; skip=%u\n",pool.highwater,ds_size(),skip);
    std::free(clip);
    return 0;
}
