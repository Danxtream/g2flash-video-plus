#include <assert.h>
#include <stdint.h>
uint64_t __aeabi_llsr(uint64_t, int);
uint64_t __aeabi_llsl(uint64_t, int);
int main(void) {
    const uint64_t values[]={0,1,UINT64_MAX,0x123456789abcdef0ULL,0x8000000000000001ULL};
    for(unsigned i=0;i<sizeof(values)/sizeof(values[0]);++i)
        for(int shift=0;shift<=64;++shift) {
            assert(__aeabi_llsr(values[i],shift)==(shift==64 ? 0 : values[i]>>shift));
            assert(__aeabi_llsl(values[i],shift)==(shift==64 ? 0 : values[i]<<shift));
        }
    return 0;
}
