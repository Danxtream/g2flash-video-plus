/* Nominal-tick bounds, not optical or synchronized clocks. SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>

typedef struct {
    uint32_t ordinal, pts, round_trip, peer_total, peer_copy, local_copy, wire_bound;
    int32_t lower, upper;
    uint32_t valid, samples, anomalies, minimum, maximum;
} video_trigger_timing;

/* Subtract only local durations. Retries and inconsistent durations remain
 * raw evidence, never reasons to abort decoding or invent a one-way delay. */
static inline void video_trigger_record(video_trigger_timing *t, uint32_t ordinal,
    uint32_t pts, uint32_t round_trip, uint32_t peer_total, uint32_t peer_copy,
    uint32_t local_copy, int retried) {
    t->ordinal = ordinal; t->pts = pts; t->round_trip = round_trip;
    t->peer_total = peer_total; t->peer_copy = peer_copy; t->local_copy = local_copy;
    t->valid = 0; t->wire_bound = UINT32_MAX; t->lower = t->upper = 0;
    if (retried || round_trip > 2000 || peer_total > 2000 || peer_copy > peer_total ||
        local_copy > 2000 || (peer_total > round_trip && peer_total - round_trip > 4)) {
        if (t->anomalies != UINT32_MAX) ++t->anomalies;
        return;
    }
    /* Four nominal milliseconds cover endpoint tick rounding. Neither clock
     * drift nor optical onset is bounded here; no symmetry is assumed. */
    t->wire_bound = (round_trip > peer_total ? round_trip - peer_total : 0) + 4;
    t->lower = (int32_t)peer_copy - (int32_t)local_copy - 4;
    t->upper = (int32_t)peer_copy - (int32_t)local_copy + (int32_t)t->wire_bound + 4;
    if (!t->samples || t->wire_bound < t->minimum) t->minimum = t->wire_bound;
    if (!t->samples || t->wire_bound > t->maximum) t->maximum = t->wire_bound;
    if (t->samples != UINT32_MAX) ++t->samples;
    t->valid = 1;
}
