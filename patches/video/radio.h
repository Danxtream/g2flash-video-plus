/* Connection-bound standard PHY adapter. SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>

enum {
    VIDEO_RADIO_LINKS = 3, /* Donor DM connection-control blocks. */
    VIDEO_RADIO_PHY_1M = 1,
    VIDEO_RADIO_PHY_2M = 2,
    VIDEO_RADIO_PHY_CODED = 3,
    VIDEO_RADIO_NONE = 0,
    VIDEO_RADIO_READ = 1,
    VIDEO_RADIO_SET = 2,
    VIDEO_RADIO_COMMAND_EVENT = 0xf3,
    VIDEO_RADIO_COMMAND_TAG = 0x5250,
    VIDEO_RADIO_DM_OPEN = 39,
    VIDEO_RADIO_DM_CLOSE = 40,
    VIDEO_RADIO_DM_READ = 68,
    VIDEO_RADIO_DM_UPDATE = 70
};

typedef struct {
    uint32_t epoch, tick, sample;
    uint32_t permission; /* A finite owner, never supplied by a wire request. */
    uint16_t handle;
    uint8_t connected, bound, tx, rx, valid, status;
} video_radio_link;

/* One queued command is enough for bounded observation and restoration. The
 * context survives disconnects; copied messages carry both epoch and serial. */
typedef struct {
    video_radio_link links[VIDEO_RADIO_LINKS];
    uint32_t serial, completed, epoch;
    uint32_t permission;
    uint8_t connection, operation, tx, rx;
    volatile uint8_t pending, result;
} video_radio_state;

/* Bind only the actual peripheral connection supplied by the stock EUS write
 * callback. No connection enumeration or global current-phone handle is used. */
void video_radio_bind(uint8_t connection);
/* Chain the original registered callbacks exactly once. The DM observer only
 * copies fixed state; it never allocates, waits, or issues a PHY command. */
void video_radio_dm_event(void *event);
void video_radio_app_handler(uint8_t events, void *message);
/* Task-context request, outside image/display locks. SET requires permission
 * on this exact epoch; READ is observational. Refusal never changes a PHY. */
int video_radio_request(uint8_t connection, uint32_t epoch, uint8_t operation,
                        uint8_t tx, uint8_t rx, uint32_t permission);
