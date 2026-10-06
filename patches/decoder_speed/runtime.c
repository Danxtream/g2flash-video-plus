#include "../memory.c"

/* Memory/compiler helpers travel with the decoder. They must never branch
 * back to the surrounding MRAM C unit when the capsule runs from RAM. */
__attribute__((noinline, no_builtin("memset")))
void *memset(void *dst, int value, size_t n) {
    unsigned char *p = dst;
    while (n--) *p++ = (unsigned char)value;
    return dst;
}
int abs(int x) { return x < 0 ? -x : x; }
void __aeabi_memcpy(void *d, const void *s, size_t n) { memcpy(d, s, n); }
void __aeabi_memcpy4(void *d, const void *s, size_t n) { memcpy(d, s, n); }
void __aeabi_memcpy8(void *d, const void *s, size_t n) { memcpy(d, s, n); }
void __aeabi_memmove(void *d, const void *s, size_t n) { memmove(d, s, n); }
void __aeabi_memmove4(void *d, const void *s, size_t n) { memmove(d, s, n); }
void __aeabi_memmove8(void *d, const void *s, size_t n) { memmove(d, s, n); }
void __aeabi_memclr(void *d, size_t n) { memset(d, 0, n); }
void __aeabi_memclr4(void *d, size_t n) { memset(d, 0, n); }
void __aeabi_memclr8(void *d, size_t n) { memset(d, 0, n); }
void __aeabi_memset(void *d, size_t n, int c) { memset(d, c, n); }

/* -Oz calls these instead of expanding 64-bit shifts. Word-sized operations
 * avoid recursively invoking the same ABI helper inside its implementation. */
uint64_t __aeabi_llsr(uint64_t value, int shift) {
    uint32_t lo = (uint32_t)value, hi = (uint32_t)(value >> 32);
    if (shift >= 64) return 0;
    if (shift >= 32) { lo = hi >> (shift - 32); hi = 0; }
    else if (shift > 0) { lo = (lo >> shift) | (hi << (32 - shift)); hi >>= shift; }
    return ((uint64_t)hi << 32) | lo;
}
uint64_t __aeabi_llsl(uint64_t value, int shift) {
    uint32_t lo = (uint32_t)value, hi = (uint32_t)(value >> 32);
    if (shift >= 64) return 0;
    if (shift >= 32) { hi = lo << (shift - 32); lo = 0; }
    else if (shift > 0) { hi = (hi << shift) | (lo >> (32 - shift)); lo <<= shift; }
    return ((uint64_t)hi << 32) | lo;
}
