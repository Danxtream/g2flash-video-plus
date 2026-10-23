/* SPDX-License-Identifier: GPL-3.0-only */
#include "timeline.h"

#pragma clang section text=".text.video"

int video_timeline_configure(video_timeline *s, uint32_t stream, uint32_t duration,
                             uint32_t pts, uint16_t numerator, uint16_t denominator) {
    if (!s || s->active || !stream || !duration || pts > duration ||
        (numerator != 10 && numerator != 15 && numerator != 20 && numerator != 24) || denominator != 1) return 0;
    *s = (video_timeline){.stream = stream, .duration = duration,
                           .start_pts = pts, .fps = numerator, .configured = 1};
    return 1;
}

int video_timeline_start(video_timeline *s, uint32_t stream, uint32_t generation) {
    if (!s || !generation || s->active || !s->configured || s->stream != stream) return 0;
    s->generation = generation;
    s->active = 1;
    return 1;
}

int video_timeline_offer_preroll(video_timeline *s, uint32_t stream, uint32_t generation,
                                 const video_frame_time *frame, uint32_t prefix) {
    if (!s || !frame || !s->active || s->stream != stream || s->generation != generation ||
        !frame->ordinal || prefix > 255 ||
        (frame->ordinal <= prefix ? frame->pts >= s->start_pts : frame->pts < s->start_pts) ||
        frame->pts > s->duration) return 0;
    for (uint32_t i = 0; i < s->count; ++i) {
        const video_frame_time *old = &s->frames[(s->head + i) % VIDEO_TIMELINE_RECORDS];
        if (old->ordinal == frame->ordinal)
            return old->nal_sequence == frame->nal_sequence && old->pts == frame->pts;
    }
    if (s->count == VIDEO_TIMELINE_RECORDS || s->accepted == UINT32_MAX ||
        frame->ordinal != s->accepted + 1 ||
        (s->accepted && (frame->nal_sequence <= s->last_nal || frame->pts < s->last_pts))) return 0;
    s->frames[(s->head + s->count) % VIDEO_TIMELINE_RECORDS] = *frame;
    ++s->count;
    s->accepted = frame->ordinal;
    s->last_nal = frame->nal_sequence;
    s->last_pts = frame->pts;
    return 1;
}

int video_timeline_offer(video_timeline *s, uint32_t stream, uint32_t generation,
                         const video_frame_time *frame) {
    return video_timeline_offer_preroll(s, stream, generation, frame, 0);
}

int video_timeline_take(video_timeline *s, uint32_t generation, uint32_t ordinal,
                        uint32_t nal_sequence, video_frame_time *out) {
    if (!s || !out || !s->active || s->generation != generation || !s->count ||
        s->consumed == UINT32_MAX || ordinal != s->consumed + 1) return 0;
    const video_frame_time *frame = &s->frames[s->head];
    if (frame->ordinal != ordinal || frame->nal_sequence != nal_sequence) return 0;
    *out = *frame;
    s->head = (s->head + 1) % VIDEO_TIMELINE_RECORDS;
    --s->count;
    s->consumed = ordinal;
    return 1;
}

void video_timeline_stop(video_timeline *s, uint32_t generation) {
    if (s && s->active && s->generation == generation) *s = (video_timeline){0};
}

#pragma clang section text=""
