/* Bounded shared-shadow presentation. SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>
#include "geometry.h"

enum {
    VIDEO_PANEL_WIDTH = 640,
    VIDEO_PANEL_HEIGHT = 480,
    VIDEO_PANEL_STRIDE = VIDEO_PANEL_WIDTH / 2,
    VIDEO_PANEL_BYTES = VIDEO_PANEL_STRIDE * VIDEO_PANEL_HEIGHT,
    VIDEO_SOURCE_Y8 = 1,
    VIDEO_SOURCE_A4 = 2,
    VIDEO_SOURCE_MENU = 3, /* Presenter-only overlay; never a media picture. */
    VIDEO_SOURCE_TYPE_MASK = 3,
    VIDEO_SOURCE_TV = 4,
    VIDEO_SOURCE_UNSIGNALLED = 8,
    VIDEO_SOURCE_DITHER = 16,
    VIDEO_SOURCE_STYLE_SHIFT = 5,
    VIDEO_SOURCE_STYLE_MASK = 96,
    VIDEO_SCALE_NATIVE = 1,
    VIDEO_SCALE_DOUBLE = 2,
    VIDEO_PRESENT_NATIVE = 1,
    VIDEO_DISPLAY_IDLE = 0,
    VIDEO_DISPLAY_QUEUED,
    VIDEO_DISPLAY_COPYING,
    VIDEO_DISPLAY_COMPLETE,
    VIDEO_DISPLAY_FAILED,
    VIDEO_DISPLAY_LIMIT_MS = 2000,
    VIDEO_DISPLAY_TAKE_LIMIT_MS = 1000
};

/* Packing consumes borrowed Y immediately. A later queued presenter can supply
 * owned A4 through the same descriptor without retaining a decoder plane. */
typedef struct {
    uint32_t token, generation, ordinal;
    uint32_t width, height, stride, bytes, format, scale;
    const uint8_t *pixels;
    video_picture_rect picture; /* Zero width/height means the entire legacy frame. */
} video_frame_descriptor;

/* Clear margins and pack top-down pixels, high nibble first. Refuse malformed
 * geometry, insufficient storage and overlap before changing the shadow. */
int video_pack_frame(uint8_t *shadow, uint32_t bytes,
                     const video_frame_descriptor *frame);
/* Stock side values differ from the transport's left/right source bits. */
uint32_t video_source_from_side(uint32_t side);

/* This mailbox lives in the stable context, never in a borrowed worker owner.
 * Only the phase crosses the display task; publication/folding uses image_mutex. */
typedef struct {
    volatile uint32_t phase, queue_failed;
    uint32_t token, generation, ordinal, copy_tick;
    uint32_t pin, stop_wait, presented, failures;
    volatile uint32_t output_epoch;
    uint32_t completed_tick; /* Paired with presented under publication protection. */
} video_presentation_state;

/* Worker task only, outside image/display locks. Return after actual copy and
 * cache flush; failure cancels, and unknown completion retains quarantine. */
int video_present_frame(const video_frame_descriptor *);
/* Existing display hook: claim a tagged job and publish completion without
 * locks, heap calls or borrowed event/owner pointers. */
uint32_t video_display_claim(void);
void video_display_complete(uint32_t token, int success);
/* Queue refusal: true only when no display copier claimed the pending job. */
int video_display_queue_failed(void);
/* Controller task, under image_mutex: fold completion and retire its pin. */
int video_display_fold_locked(void);
