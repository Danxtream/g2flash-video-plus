/* No second panel allocation. SPDX-License-Identifier: GPL-3.0-only */
#include "menu.h"
#pragma clang section text=".text.video.menu"
#ifndef VIDEO_MENU_TEXT
#define VIDEO_MENU_TEXT(shadow, text, bytes, rectangles) \
    cfw_builtin_draw_string(shadow, VIDEO_PANEL_STRIDE, VIDEO_PANEL_WIDTH, VIDEO_MENU_HEIGHT, text, bytes, rectangles)
#endif
static uint32_t video_menu_number(uint8_t *out, uint32_t value, uint32_t minimum) {
    uint8_t digits[10]; uint32_t n = 0;
    do { digits[n++] = '0' + value % 10; value /= 10; } while (value);
    while (n < minimum) digits[n++] = '0';
    for (uint32_t i = 0; i < n; ++i) out[i] = digits[n - i - 1];
    return n;
}
static uint32_t video_menu_time(uint8_t *out, uint32_t milliseconds) {
    uint32_t seconds = milliseconds / 1000, n = 0;
    n += video_menu_number(out + n, seconds / 3600, 1); out[n++] = ':';
    n += video_menu_number(out + n, seconds / 60 % 60, 2); out[n++] = ':';
    n += video_menu_number(out + n, seconds % 60, 2); return n;
}
static int video_menu_draw(uint8_t *shadow, uint32_t view, uint32_t position, uint32_t duration) {
    if (!shadow || view > 9 || (view && !(view & 1)) || position > duration) return 0;
    uint8_t *band = shadow + (VIDEO_PANEL_HEIGHT - VIDEO_MENU_HEIGHT) * VIDEO_PANEL_STRIDE;
    bzero(band, VIDEO_MENU_HEIGHT * VIDEO_PANEL_STRIDE);
    if (!view) return 1;
    uint8_t text[40] = {96, 0, 2, 0, VIDEO_MENU_TEXT_LEVEL, 0};
    uint32_t n = video_menu_time(text + 6, position);
    text[6 + n++] = ' '; text[6 + n++] = '/'; text[6 + n++] = ' ';
    n += video_menu_time(text + 6 + n, duration); text[5] = n;
    cfw_rectlist rectangles = {0};
    if (VIDEO_MENU_TEXT(band, text, 6 + n, &rectangles)) return 0;
    static const char choices[VIDEO_MENU_CHOICES][12] = {"PLAY", "-10 seconds", "+10 seconds", "-30 seconds", "+30 seconds"};
    const char *choice = choices[view >> 1];
    text[0] = 240; text[1] = 0; text[2] = 24; n = 0;
    while (choice[n]) { text[6 + n] = choice[n]; ++n; }
    text[5] = n;
    return VIDEO_MENU_TEXT(band, text, 6 + n, &rectangles) == 0;
}
#pragma clang section text=""
