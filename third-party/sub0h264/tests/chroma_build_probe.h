/** Test ABI for comparing separately compiled decoder configurations.
 *  SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdint.h>

typedef struct {
    const uint8_t *y, *contexts;
    const uint32_t *syntax;
    uint32_t width, height, stride, frames, bit_offset, syntax_words;
    uint32_t entered, completed, has_chroma, context_bytes;
} chroma_probe_view;

#ifdef __cplusplus
extern "C" {
#endif

/* Own parsed NALs and a fresh decoder. capture records entropy trace events. */
void *chroma_reference_create(const uint8_t *, uint32_t, int skip, int capture);
void *chroma_trimmed_create(const uint8_t *, uint32_t, int skip, int capture);
/* Feed one pre-parsed NAL; -1 denotes a parse failure, other values DecodeStatus. */
int chroma_reference_step(void *, uint32_t);
int chroma_trimmed_step(void *, uint32_t);
/* Inspect the current borrowed Y/entropy state, valid until step or destroy. */
chroma_probe_view chroma_reference_view(void *);
chroma_probe_view chroma_trimmed_view(void *);
uint32_t chroma_reference_nals(void *);
uint32_t chroma_trimmed_nals(void *);
void chroma_reference_destroy(void *);
void chroma_trimmed_destroy(void *);
/* Check absent planes, allocation preflight, DPB reuse and deblocking thresholds. */
int chroma_trimmed_storage_check(void);

#ifdef __cplusplus
}
#endif
