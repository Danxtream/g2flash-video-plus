/* RIGHT-selected FIFO trigger state. SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>
#include "timeline.h"
enum {
    VIDEO_SYNC_BRIDGE = 5,
    VIDEO_SYNC_BYTES = 24,
    VIDEO_SYNC_MAIL_ROWS = 4,
    VIDEO_SYNC_READY = 1,
    VIDEO_SYNC_TRIGGER = 2,
    VIDEO_SYNC_DONE = 3,
    VIDEO_SYNC_PAUSE = 4,
    VIDEO_SYNC_RESUME = 5,
    VIDEO_SYNC_MENU = 6
};
typedef struct {
    uint32_t stream, side, next, inflight, triggered, completed, peer_completed, flight_pts;
    uint32_t local_ordinal, local_pts, local_depth, local_terminal;
    uint32_t peer_ordinal, peer_pts, peer_depth, peer_terminal, fault;
} video_sync;
typedef struct {
    volatile uint32_t high, low, overflow;
    uint8_t rows[VIDEO_SYNC_MAIL_ROWS][VIDEO_SYNC_BYTES];
    uint32_t captured[VIDEO_SYNC_MAIL_ROWS];
} video_sync_mail;
/* Initialize a finite stream; lens bits are1 LEFT and2 RIGHT. */
int video_sync_init(video_sync *, uint32_t stream, uint32_t side);
/* Publish this lens's decoded head, never input or decoder sequence credits. */
int video_sync_ready(video_sync *, uint32_t ordinal, uint32_t pts, uint32_t depth, int terminal);
/* Process a wire-only peer record on the presenter task; repeats are idempotent. */
int video_sync_receive(video_sync *, uint32_t source, uint32_t stream, uint32_t operation,
                        uint32_t ordinal, uint32_t pts, uint32_t depth, int terminal);
/* RIGHT can issue a trigger only for matching ready heads and completed predecessor. */
int video_sync_trigger(video_sync *);
/* Mark actual local display-copy completion, not submission. */
int video_sync_complete(video_sync *, uint32_t ordinal);
/* Wire callback only copies a bounded record and posts stable controller work. */
uint32_t video_sync_received(const uint8_t *, uint32_t);
void video_control_notify_sync(void);
