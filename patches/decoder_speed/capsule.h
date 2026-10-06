#pragma once
#include <stddef.h>
#include <stdint.h>

/* This slot belongs to jim's already-reserved 1 KiB CFW tail. A separate
 * import table makes the capsule's internal references movable as a unit. */
#ifndef DS_IMPORT_SLOT
#define DS_IMPORT_SLOT 0x2029f4b0U
#endif
typedef struct { uint32_t tag, size; } ds_request;
typedef struct { uint32_t tag, size; } ds_failure;
typedef struct {
    void *(*alloc)(uint32_t);
    void (*release)(void *);
    int (*preflight)(const ds_request *, uint32_t, ds_failure *);
    void (*fail)(uint32_t);
    void (*profile_enter)(uint32_t);
    void (*profile_exit)(uint32_t);
} ds_imports;
typedef struct {
    const uint8_t *y;
    uint32_t width, height, stride, count;
} ds_frame_info;
#ifdef __cplusplus
extern "C" {
#endif
void ds_profile_enter(uint32_t id);
void ds_profile_exit(uint32_t id);
uint32_t ds_size(void);
uint32_t ds_selftest(uint32_t value);
void *ds_init(void *memory, uint32_t size, uint32_t skip);
void ds_destroy(void *handle);
int ds_decode(void *handle, const uint8_t *nal, uint32_t size);
int ds_frame(const void *handle, ds_frame_info *info);
#ifdef __cplusplus
}
#endif
