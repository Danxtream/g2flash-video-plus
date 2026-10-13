/* Bounded selection and existing raw bitmap font. SPDX-License-Identifier: GPL-3.0-only */
#include "menu.h"
#pragma clang section text=".text.video.menu"
uint32_t video_menu_bits(const video_menu *menu) {
    return menu->open ? 1u | menu->selection << 1 : 0;
}
int video_menu_action(video_menu *menu, uint32_t action, uint32_t position, uint32_t duration) {
    if (!menu || action > VIDEO_MENU_QUERY || menu->selection >= VIDEO_MENU_CHOICES || position > duration) return 0;
    if (action == VIDEO_MENU_OPEN) { menu->open = 1; menu->selection = 0; return 1; }
    if (action == VIDEO_MENU_CLOSE) { menu->open = menu->selection = 0; return 1; }
    if (action == VIDEO_MENU_QUERY) return 1;
    if (!menu->open) return 0;
    if (action == VIDEO_MENU_NEXT || action == VIDEO_MENU_PREVIOUS) {
        menu->selection = (menu->selection + (action == VIDEO_MENU_NEXT ? 1 : VIDEO_MENU_CHOICES - 1)) % VIDEO_MENU_CHOICES;
        return 1;
    }
    if (!menu->selection) { menu->open = 0; return 2; }
    if (menu->seek_serial == UINT32_MAX) return 0;
    const int32_t steps[VIDEO_MENU_CHOICES] = {0, -10, 10, -30, 30};
    int64_t target = (int64_t)position + steps[menu->selection] * 1000;
    menu->seek_target = target <= 0 ? 0 : target >= duration ? duration : (uint32_t)target;
    ++menu->seek_serial;
    return 1;
}

#pragma clang section text=""
