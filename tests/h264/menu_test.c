/* Bounded pause choices and seek arithmetic. SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include "../../patches/video/menu.c"
int main(void) {
    video_menu menu = {0};
    assert(!video_menu_action(&menu, VIDEO_MENU_NEXT, 0, 1000));
    assert(video_menu_action(&menu, VIDEO_MENU_OPEN, 0, 1000) == 1);
    assert(video_menu_bits(&menu) == 1);
    for (uint32_t i = 0; i < VIDEO_MENU_CHOICES; ++i)
        assert(video_menu_action(&menu, VIDEO_MENU_NEXT, 0, 1000));
    assert(!menu.selection);
    assert(video_menu_action(&menu, VIDEO_MENU_PREVIOUS, UINT32_MAX - 2, UINT32_MAX));
    assert(menu.selection == 4);
    assert(video_menu_action(&menu, VIDEO_MENU_APPLY, UINT32_MAX - 2, UINT32_MAX));
    assert(menu.seek_target == UINT32_MAX && menu.seek_serial == 1);
    menu.selection = 1;
    assert(video_menu_action(&menu, VIDEO_MENU_APPLY, 3, UINT32_MAX));
    assert(!menu.seek_target && menu.seek_serial == 2);
    menu.seek_serial = UINT32_MAX;
    assert(!video_menu_action(&menu, VIDEO_MENU_APPLY, 3, UINT32_MAX));
    menu.selection = 0;
    assert(video_menu_action(&menu, VIDEO_MENU_APPLY, 3, UINT32_MAX) == 2 && !menu.open);
    assert(!video_menu_action(&menu, VIDEO_MENU_OPEN, 2, 1));
    return 0;
}
