/* Missing compiler/C++ helpers. SPDX-License-Identifier: GPL-3.0-only */
#include "runtime.h"
#include <stddef.h>

/* memcpy/memmove are shared with jim's existing C unit; do not duplicate them.
 * No startup constructors, writable globals or fixed firmware addresses. */
extern void *memcpy(void *, const void *, size_t);
extern void *memmove(void *, const void *, size_t);

__attribute__((noinline, no_builtin("memset")))
void *memset(void *destination, int value, size_t size) {
    unsigned char *p = destination;
    while (size--) *p++ = (unsigned char)value;
    return destination;
}
int abs(int value) { return value < 0 ? -value : value; }

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
void __aeabi_memset4(void *d, size_t n, int c) { memset(d, c, n); }
void __aeabi_memset8(void *d, size_t n, int c) { memset(d, c, n); }

/* -Oz uses these for variable 64-bit shifts. Word operations avoid recursively
 * calling the helper being implemented; shifts >=64 are defined here as zero. */
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

__attribute__((weak))
const g2_h264_runtime *g2_h264_runtime_current(void) { return NULL; }
