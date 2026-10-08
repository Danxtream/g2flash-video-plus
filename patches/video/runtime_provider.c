/* SPDX-License-Identifier: GPL-3.0-only */
#include "../cfw_context.h"

/* Read only the existing singleton: decoder queries cannot allocate at boot.
 * Cancellation does not unbind callbacks needed by construction/destruction. */
const g2_h264_runtime *g2_h264_runtime_current(void) {
    customCfwContext *ctx = peekCustomCfwContext();
    return ctx ? __atomic_load_n(&ctx->video_runtime, __ATOMIC_ACQUIRE) : 0;
}
