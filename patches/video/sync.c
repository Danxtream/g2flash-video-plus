/* No local clock comparison or scheduled-time arming. SPDX-License-Identifier: GPL-3.0-only */
#include "sync.h"

#pragma clang section text=".text.video"
int video_sync_init(video_sync *s, uint32_t stream, uint32_t side) {
    if (!s || !stream || (side != 1 && side != 2)) return 0;
    *s = (video_sync){.stream = stream, .side = side, .next = 1}; return 1;
}
int video_sync_ready(video_sync *s, uint32_t ordinal, uint32_t pts, uint32_t depth, int terminal) {
    if (!s || s->fault || !ordinal || ordinal < s->next || ordinal > s->next + (s->inflight != 0) ||
        !depth || depth > 2) return 0;
    s->local_ordinal = ordinal; s->local_pts = pts; s->local_depth = depth;
    s->local_terminal = terminal != 0; return 1;
}
int video_sync_receive(video_sync *s, uint32_t source, uint32_t stream, uint32_t op,
                        uint32_t ordinal, uint32_t pts, uint32_t depth, int terminal) {
    if (!s || s->fault || source != (s->side ^ 3) || stream != s->stream || !ordinal) return 0;
    if (ordinal < s->next) return 1;
    if (op == VIDEO_SYNC_READY) {
        if (!depth || depth > 2 || ordinal > s->next + (s->inflight != 0)) return 0;
        if (s->peer_ordinal == ordinal && s->peer_depth && s->peer_pts != pts) return 0;
        s->peer_ordinal = ordinal; s->peer_pts = pts; s->peer_depth = depth;
        s->peer_terminal = terminal != 0; return 1;
    }
    if (op == VIDEO_SYNC_TRIGGER) {
        if (s->side != 1 || source != 2 || ordinal != s->next || depth ||
            ordinal != s->local_ordinal || pts != s->local_pts) return 0;
        s->inflight = s->triggered = ordinal; s->flight_pts = pts; return 1;
    }
    if (op == VIDEO_SYNC_DONE) {
        if (s->side != 2 || ordinal != s->inflight || depth || pts != s->flight_pts) return 0;
        s->peer_completed = ordinal; return 1;
    }
    return 0;
}
int video_sync_trigger(video_sync *s) {
    if (!s || s->fault || s->side != 2) return 0;
    if (s->inflight) {
        if (s->completed != s->inflight || s->peer_completed != s->inflight) return 0;
        if (s->next == UINT32_MAX) { s->fault = 1; return 0; }
        ++s->next; s->inflight = 0;
    }
    if (s->local_ordinal != s->next || s->peer_ordinal != s->next || s->local_pts != s->peer_pts ||
        !s->local_depth || !s->peer_depth) return 0;
    if (!s->triggered && !((s->local_depth == 2 && s->peer_depth == 2) ||
                           (s->local_terminal && s->peer_terminal))) return 0;
    s->inflight = s->triggered = s->next; s->flight_pts = s->local_pts; return 1;
}
int video_sync_complete(video_sync *s, uint32_t ordinal) {
    if (!s || !ordinal || ordinal != s->inflight || s->completed >= ordinal) return 0;
    s->completed = ordinal;
    if (s->side == 1) {
        if (s->next == UINT32_MAX) { s->fault = 1; return 0; }
        ++s->next; s->inflight = 0;
    }
    return 1;
}
#pragma clang section text=""
