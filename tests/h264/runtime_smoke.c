/* Check compiler helpers without ARM hardware. SPDX-License-Identifier: GPL-3.0-only */
#include "../../patches/h264/runtime.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint64_t __aeabi_llsr(uint64_t, int);
uint64_t __aeabi_llsl(uint64_t, int);
void __aeabi_memcpy8(void *, const void *, size_t);
void __aeabi_memmove8(void *, const void *, size_t);
void __aeabi_memset8(void *, size_t, int);
void __aeabi_memclr8(void *, size_t);

#define CHECK(value) do { if (!(value)) { \
    fprintf(stderr, "runtime check failed at %d\n", __LINE__); exit(1); \
} } while (0)

int main(void) {
    CHECK(!g2_h264_runtime_current());
    uint64_t values[] = {0, 1, UINT64_MAX, UINT64_C(0x123456789abcdef0)};
    for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        for (int shift = 0; shift <= 70; ++shift) {
            CHECK(__aeabi_llsr(values[i], shift) == (shift < 64 ? values[i] >> shift : 0));
            CHECK(__aeabi_llsl(values[i], shift) == (shift < 64 ? values[i] << shift : 0));
        }
    }
    unsigned char input[96], actual[96], expected[96];
    for (unsigned i = 0; i < sizeof(input); ++i) input[i] = (unsigned char)(i * 3);
    for (unsigned offset = 0; offset < 8; ++offset) {
        for (unsigned size = 0; size < 64; ++size) {
            memset(actual, 0, sizeof(actual)); memset(expected, 0, sizeof(expected));
            __aeabi_memcpy8(actual + offset, input, size);
            memcpy(expected + offset, input, size);
            CHECK(!memcmp(actual, expected, sizeof(actual)));
            __aeabi_memset8(actual + offset, size, 0xa5);
            memset(expected + offset, 0xa5, size);
            CHECK(!memcmp(actual, expected, sizeof(actual)));
            __aeabi_memclr8(actual + offset, size);
            memset(expected + offset, 0, size);
            CHECK(!memcmp(actual, expected, sizeof(actual)));
            memcpy(actual, input, sizeof(input)); memcpy(expected, input, sizeof(input));
            __aeabi_memmove8(actual + offset, actual, size);
            memmove(expected + offset, expected, size);
            CHECK(!memcmp(actual, expected, sizeof(actual)));
            __aeabi_memmove8(actual, actual + offset, size);
            memmove(expected, expected + offset, size);
            CHECK(!memcmp(actual, expected, sizeof(actual)));
        }
    }
    puts("EABI helpers PASS");
}
