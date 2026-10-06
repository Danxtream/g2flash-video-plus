#pragma once
#include <stdint.h>

/* Frozen SPS/PPS + 32-frame clips, verified on the PC before firmware build.
 * Only metadata is embedded. Clip bytes and Y references stay private. */
#define DS_CLIP_BYTES 23921U
#define DS_CLIP_CRC 0xc81c1bdcU
#define DS_CLIP_MAX_BYTES 23921U
typedef struct { uint32_t bytes, crc; } ds_clip_profile;
static const ds_clip_profile ds_clips[] = {
    {DS_CLIP_BYTES, DS_CLIP_CRC}, /* Original Tokyo segment. */
    {22888U, 0xa7e2ccf1U},       /* Matching re-encode, deblocking on. */
    {23296U, 0x65ebfb75U},       /* Matching re-encode, deblocking off. */
};
static inline int ds_clip_index(uint32_t bytes, uint32_t crc) {
    for (uint32_t i=0; i<sizeof(ds_clips)/sizeof(ds_clips[0]); ++i)
        if (ds_clips[i].bytes==bytes && ds_clips[i].crc==crc) return (int)i;
    return -1;
}
