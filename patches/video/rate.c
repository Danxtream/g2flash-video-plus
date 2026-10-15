/* Presenter-owned windows, with bounded worker edge posts. SPDX-License-Identifier: GPL-3.0-only */
#include "rate.h"
#include "../memory.h"
#pragma clang section text=".text.video"

void video_rate_start(video_rate_state *s, uint32_t stream, uint32_t generation, uint32_t token) {
    *s = (video_rate_state){0};
    s->stream = stream; s->generation = generation; s->token = token;
    s->mode = VIDEO_RATE_INPUT; s->posted_mode = VIDEO_RATE_INPUT;
}
void video_rate_post_worker(video_rate_state *s, uint32_t tick, uint32_t code) {
    if (!s->generation || (s->flags & VIDEO_RATE_ENDED)) return;
    if (code <= VIDEO_RATE_OUTPUT && code == s->posted_mode) return;
    if (s->count == VIDEO_RATE_POSTS) { s->flags |= VIDEO_RATE_INVALID; return; }
    s->posts[(s->head + s->count) % VIDEO_RATE_POSTS] = (video_rate_post){tick, code};
    ++s->count;
    if (code <= VIDEO_RATE_OUTPUT) s->posted_mode = code;
}
static uint32_t video_rate_cause(const video_rate_state *s) {
    if (s->paused) return VIDEO_RATE_PAUSED;
    if (s->ready) return VIDEO_RATE_PRESENT;
    return s->mode == VIDEO_RATE_INPUT ? VIDEO_RATE_EMPTY :
        s->mode == VIDEO_RATE_DECODE ? VIDEO_RATE_SLOW : VIDEO_RATE_WAIT;
}
static void video_rate_advance(video_rate_state *s, uint32_t now) {
    if (!(s->flags & VIDEO_RATE_ACTIVE) || (s->flags & VIDEO_RATE_ENDED)) return;
    uint32_t delta = now - s->tick;
    if (delta >= 0x80000000U) { s->flags |= VIDEO_RATE_INVALID; return; }
    /* Never spend an unbounded loop catching up after a long scheduling gap. */
    if (delta > VIDEO_RATE_ROWS * 1000) {
        uint32_t skip = (delta - 1000) / 1000;
        if (skip >= UINT32_MAX - s->high) { s->flags |= VIDEO_RATE_INVALID; return; }
        s->high += skip; s->tick += skip * 1000;
        s->current = (video_rate_window){.start = s->high * 1000};
        s->flags |= VIDEO_RATE_INVALID; delta = now - s->tick;
    }
    while (delta) {
        uint32_t offset = (s->tick - s->anchor) % 1000;
        uint32_t part = delta < 1000 - offset ? delta : 1000 - offset;
        video_rate_window *w = &s->current;
        if (s->mode == VIDEO_RATE_INPUT) w->input += part;
        if (s->mode == VIDEO_RATE_OUTPUT) w->output += part;
        if (s->mode == VIDEO_RATE_DECODE) {
            w->service += part;
            if (s->frame_ms <= UINT32_MAX - part) s->frame_ms += part;
            else s->flags |= VIDEO_RATE_INVALID;
        }
        if (!s->ready) w->not_ready += part;
        uint32_t cause = video_rate_cause(s);
        w->flags |= cause; s->gap_flags |= cause;
        if (s->flags & VIDEO_RATE_INVALID) w->flags |= VIDEO_RATE_LOSS;
        s->tick += part; delta -= part;
        uint32_t gap = s->tick - s->copy_tick;
        if (gap > w->gap) w->gap = gap;
        if (part == 1000 - offset) {
            if (s->high == UINT32_MAX) { s->flags |= VIDEO_RATE_INVALID; return; }
            w->serial = ++s->high; w->ordinal = s->ordinal;
            s->rows[(s->high - 1) % VIDEO_RATE_ROWS] = *w;
            *w = (video_rate_window){.start = s->high * 1000};
        }
    }
}
static void video_rate_fold(video_rate_state *s, uint32_t now) {
    while (s->count) {
        video_rate_post *p = &s->posts[s->head];
        if ((int32_t)(now - p->tick) < 0) break;
        video_rate_advance(s, p->tick);
        if (p->code <= VIDEO_RATE_OUTPUT) s->mode = p->code;
        else if (p->code == VIDEO_RATE_FRAME) {
            if (s->frame_ms > s->current.decode_max) s->current.decode_max = s->frame_ms;
            if (s->frame_ms > s->gap_decode) s->gap_decode = s->frame_ms;
            /* Millisecond quantization makes 63 ms borderline, not proof. */
            if (s->frame_ms >= 64) ++s->current.overrun;
            s->frame_ms = 0;
        } else if (p->code == VIDEO_RATE_READY) s->ready = 1;
        s->head = (s->head + 1) % VIDEO_RATE_POSTS; --s->count;
    }
    video_rate_advance(s, now);
}
void video_rate_present(video_rate_state *s, uint32_t tick, uint32_t ready, uint32_t paused) {
    video_rate_fold(s, tick); s->ready = !!ready; s->paused = !!paused;
}
void video_rate_copy(video_rate_state *s, uint32_t tick, uint32_t ordinal, uint32_t terminal) {
    if (!s->generation || (s->flags & VIDEO_RATE_ENDED)) return;
    if (!(s->flags & VIDEO_RATE_ACTIVE)) {
        video_rate_fold(s, tick);
        s->anchor = s->tick = s->copy_tick = tick;
        s->flags |= VIDEO_RATE_ACTIVE;
    }
    video_rate_fold(s, tick);
    uint32_t gap = tick - s->copy_tick;
    if (s->ordinal) {
        const uint32_t limits[] = {0, 15, 31, 47, 61, 62, 63, 64, 80, 100, 125, 250, 500, 1000, 5000, UINT32_MAX};
        uint32_t bin = 0;
        while (bin < 15 && gap > limits[bin]) ++bin;
        if (s->gap_count == UINT32_MAX || s->gaps[bin] == UINT16_MAX) s->flags |= VIDEO_RATE_INVALID;
        else { ++s->gaps[bin]; ++s->gap_count; }
        if (gap > s->gap_max) s->gap_max = gap;
    }
    if (gap > 64 && s->dip_high != UINT32_MAX) {
        video_rate_dip *d = &s->dips[s->dip_high % VIDEO_RATE_ROWS];
        *d = (video_rate_dip){++s->dip_high, s->copy_tick - s->anchor,
            tick - s->anchor, s->ordinal, ordinal, gap, s->gap_flags, s->gap_decode};
    }
    if (gap > s->current.gap) s->current.gap = gap;
    uint32_t phase = ((tick - s->anchor) % 1000) / 100;
    uint32_t *packed = phase < 8 ? &s->current.phase_lo : &s->current.phase_hi;
    uint32_t shift = (phase % 8) * 4;
    if (((*packed >> shift) & 15) == 15) s->flags |= VIDEO_RATE_INVALID;
    else *packed += 1u << shift;
    ++s->current.shown; s->current.ordinal = s->ordinal = ordinal;
    s->copy_tick = tick; s->gap_flags = s->gap_decode = 0;
    if (terminal) s->flags |= VIDEO_RATE_ENDED;
}
void video_rate_stop(video_rate_state *s, uint32_t tick) {
    video_rate_fold(s, tick); s->flags |= VIDEO_RATE_ENDED;
}
static void video_rate_write(uint8_t *p, uint32_t value) {
    for (uint32_t i = 0; i < 4; ++i) p[i] = value >> (i * 8);
}
int video_rate_snapshot(const video_rate_state *s, uint32_t mode, uint32_t cursor, uint8_t lens, uint8_t *out) {
    if (mode > 2 || !s->generation) return 0;
    for (uint32_t i = 0; i < (mode ? 128u : 144u); ++i) out[i] = 0;
    out[0] = 1; out[1] = 7; out[3] = VIDEO_RATE_DIAGNOSTICS;
    out[4] = 1; out[5] = mode; out[6] = lens; out[7] = s->flags;
    video_rate_write(out + 8, s->stream); video_rate_write(out + 12, s->generation);
    video_rate_write(out + 16, s->token); video_rate_write(out + 20, s->anchor);
    if (mode == 2) {
        video_rate_write(out + 24, s->gap_count); out[28] = 16; out[29] = 4;
        for (uint32_t i = 0; i < 16; ++i) video_rate_write(out + 32 + 4 * i, s->gaps[i]);
        video_rate_write(out + 96, s->gap_max);
        return 1;
    }
    uint32_t high = mode ? s->dip_high : s->high;
    video_rate_write(out + 24, high);
    uint32_t first = high > VIDEO_RATE_ROWS ? high - VIDEO_RATE_ROWS + 1 : 1;
    uint32_t serial = cursor < first ? first : cursor;
    uint32_t bytes = mode ? sizeof(video_rate_dip) : sizeof(video_rate_window);
    out[29] = bytes;
    while (serial <= high && out[28] < (mode ? 96u : 112u) / bytes) {
        const uint8_t *row = mode ? (const uint8_t *)&s->dips[(serial - 1) % VIDEO_RATE_ROWS] :
            (const uint8_t *)&s->rows[(serial - 1) % VIDEO_RATE_ROWS];
        /* Encode explicitly: tests and consumers never depend on host endian. */
        for (uint32_t i = 0; i < bytes; i += 4) {
            uint32_t word; memcpy(&word, row + i, 4);
            video_rate_write(out + 32 + out[28] * bytes + i, word);
        }
        ++out[28]; if (serial == UINT32_MAX) break; ++serial;
    }
    return 1;
}
#pragma clang section text=""
