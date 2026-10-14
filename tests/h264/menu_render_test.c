/* Real text LUT/rendering with a bounded font fixture. SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../../patches/cfw_context.h"
#include "../../patches/debug.h"

static const uint8_t test_font[20] = {[12] = 20};
static int test_font_dsc(const void *, void *, uint32_t, uint32_t);
static const uint8_t *test_font_bitmap(void *, void *);
static void test_font_release(void *);
static uint32_t rd16(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | rd16(p + 2) << 16; }
static void *cfw_malloc(uint32_t bytes) { return malloc(bytes); }
#define FW_FREE free
static customCfwContext *getCustomCfwContext(void) { return 0; }
int cfw_fb_lease_active(void) { return 1; }
static void rl_add(cfw_rectlist *rl, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (rl->n < CFW_RECT_MAX) rl->r[rl->n++] = (cfw_rect){x, y, w, h};
}

/* The test substitutes only absolute font entry points, never the text/LUT path. */
#include "menu_texture_fixture.h"
#include "../../patches/video/menu_draw.c"

static void write16(uint8_t *p, uint32_t value) { p[0] = value; p[1] = value >> 8; }
static int test_font_dsc(const void *font, void *output, uint32_t letter, uint32_t next) {
    assert(font == test_font && letter >= 32 && letter < 127 && next < 127);
    uint8_t *dsc = output;
    dsc[0] = 1;
    write16(dsc + CFW_GLYPH_ADV_W, 6);
    if (letter != ' ') {
        write16(dsc + CFW_GLYPH_BOX_W, 4);
        write16(dsc + CFW_GLYPH_BOX_H, 4);
    }
    dsc[CFW_GLYPH_FORMAT] = CFW_GLYPH_A4_ALIGNED;
    return 1;
}
static const uint8_t *test_font_bitmap(void *descriptor, void *draw_buffer) {
    const uint8_t *dsc = descriptor;
    assert(dsc[CFW_GLYPH_RAW_FLAG] & 1);
    assert(draw_buffer);
    static const uint8_t bitmap[] = {
        0xf0, 0x0f, 0, 0xf0, 0x0f, 0, 0xf0, 0x0f, 0, 0xf0, 0x0f, 0
    };
    return bitmap;
}
static void test_font_release(void *descriptor) { assert(descriptor); }
static uint32_t lit(const uint8_t *shadow, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    uint32_t count = 0;
    for (uint32_t dy = 0; dy < h; ++dy) for (uint32_t dx = 0; dx < w; ++dx) {
        uint32_t column = x + dx;
        uint8_t byte = shadow[(y + dy) * VIDEO_PANEL_STRIDE + column / 2];
        uint8_t level = (column & 1) ? byte & 15 : byte >> 4;
        assert(level == 0 || level == 15);
        count += level != 0;
    }
    return count;
}
int main(void) {
    uint8_t *guarded = malloc(VIDEO_PANEL_BYTES + 2);
    assert(guarded);
    uint8_t *shadow = guarded + 1;
    uint32_t band_y = VIDEO_PANEL_HEIGHT - VIDEO_MENU_HEIGHT;
    for (uint32_t view = 1; view <= 9; view += 2) {
        memset(guarded, 0x55, VIDEO_PANEL_BYTES + 2);
        assert(video_menu_draw(shadow, view, 3620000, 7320000));
        assert(lit(shadow, 96, band_y + 18, 156, 4) > 0);
        assert(lit(shadow, 240, band_y + 40, 66, 4) > 0);
        for (uint32_t i = 0; i < band_y * VIDEO_PANEL_STRIDE; ++i) assert(shadow[i] == 0x55);
        assert(guarded[0] == 0x55 && guarded[VIDEO_PANEL_BYTES + 1] == 0x55);
        assert(video_menu_draw(shadow, 0, 3620000, 7320000));
        for (uint32_t i = band_y * VIDEO_PANEL_STRIDE; i < VIDEO_PANEL_BYTES; ++i) assert(!shadow[i]);
    }
    free(guarded);
    return 0;
}
