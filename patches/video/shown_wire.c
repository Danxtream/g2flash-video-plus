/* Frozen per-lens telemetry without a playback ACK. SPDX-License-Identifier: GPL-3.0-only */
#include "shown.h"
#pragma clang section text=".text.video"
static int video_shown_snapshot_locked(customCfwContext *ctx, const uint8_t *data,
                                         uint8_t origin, uint8_t *out) {
    video_owner *owner = ctx->video_owner;
    if (!owner || !owner->presenter || ctx->video.preparing || !ctx->video.published ||
        origin != ctx->video_control.owner_origin || video_read32(data + 8) != ctx->video_control.stream ||
        video_read32(data + 12) != owner->token || !ctx->video_control.start_guard)
        return VIDEO_CONTROL_STALE;
    video_presenter *p = owner->presenter;
    video_shown_ring *r = &p->shown_rows;
    uint32_t cursor = video_read32(data + 16), serial;
    video_shown_frame row;
    out[3] = ctx->video_control.owner_here;
    video_write32(out + 4, ctx->video_control.stream); video_write32(out + 8, owner->token);
    video_write32(out + 40, r->first); video_write32(out + 44, r->high);
    video_write32(out + 48, r->lost); video_write32(out + 52, p->holds);
    /* No shared-clock subtraction or invented wire measurement. The local
     * trigger-to-copy delta is useful; inter-lens offset stays unqualified
     * until loaded transport compensation/uncertainty is established. */
    video_write32(out + 60, p->paired ? UINT32_MAX : 0);
    if (!video_shown_read(r, cursor, &row, &serial)) return VIDEO_CONTROL_BUSY;
    video_write32(out + 12, serial); video_write32(out + 16, row.ordinal);
    video_write32(out + 20, row.pts); video_write32(out + 24, row.copy_tick);
    video_write32(out + 28, row.trigger_tick); video_write32(out + 32, row.queued_tick);
    video_write32(out + 36, row.flags | (cursor && cursor < r->first ? VIDEO_SHOWN_RECOVERED : 0u));
    return VIDEO_CONTROL_ACCEPTED;
}
#pragma clang section text=""
