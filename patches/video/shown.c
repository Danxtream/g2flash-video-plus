/* Capture timestamps are not processing or optical onset times. SPDX-License-Identifier: GPL-3.0-only */
#include "shown.h"
#pragma clang section text=".text.video"
int video_shown_publish(video_shown_ring *r, const video_shown_frame *frame) {
    if (!r || !frame || !frame->ordinal || r->high == UINT32_MAX ||
        (r->high && frame->ordinal <= r->rows[(r->high - 1) % VIDEO_SHOWN_ROWS].ordinal)) return 0;
    uint32_t serial = r->high + 1;
    r->rows[(serial - 1) % VIDEO_SHOWN_ROWS] = *frame;
    r->high = serial;
    r->first = serial > VIDEO_SHOWN_ROWS ? serial - VIDEO_SHOWN_ROWS + 1 : 1;
    r->lost = r->first - 1;
    return 1;
}
int video_shown_read(const video_shown_ring *r, uint32_t cursor,
                      video_shown_frame *frame, uint32_t *serial) {
    if (!r || !frame || !serial || !r->high || cursor > r->high) return 0;
    uint32_t selected = cursor < r->first ? r->first : cursor;
    *frame = r->rows[(selected - 1) % VIDEO_SHOWN_ROWS]; *serial = selected; return 1;
}
#pragma clang section text=""
