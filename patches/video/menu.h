/* PC-driven paused playback selection. SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>
enum {
    VIDEO_CONTROL_MENU = 19, VIDEO_CONTROL_SCALE = 20,
    VIDEO_MENU_STATE_BYTES = 48,
    VIDEO_MENU_OPEN = 0, VIDEO_MENU_CLOSE, VIDEO_MENU_NEXT, VIDEO_MENU_PREVIOUS,
    VIDEO_MENU_APPLY, VIDEO_MENU_QUERY,
    VIDEO_MENU_CHOICES = 5, VIDEO_MENU_HEIGHT = 48,
    VIDEO_MENU_TEXT_LEVEL = 15
};
typedef struct {
    uint32_t open, selection, seek_serial, seek_target;
} video_menu;
/* Presenter-only action:1 accepted,2 resume PLAY,0 invalid/exhausted. */
int video_menu_action(video_menu *, uint32_t action, uint32_t position, uint32_t duration);
/* Compact common view; seek requests belong to the PC owning the source file. */
uint32_t video_menu_bits(const video_menu *);
