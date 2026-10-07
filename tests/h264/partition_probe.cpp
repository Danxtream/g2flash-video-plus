/* Test-only entry for clips exceeding the firmware NAL bound.
 * SPDX-License-Identifier: GPL-3.0-only */
#include "../../patches/h264/g2_h264.cpp"

/* Compile this translation unit both whole and partitioned. Only the test entry
 * bypasses admission; the production C ABI retains its 4086-byte NAL limit. */
extern "C" g2_h264_result h264_partition_test_decode(void *handle,
                                                     const uint8_t *data, uint32_t size) {
    auto *state = static_cast<State *>(handle);
    state->frameReady = false;
    if (!sub0h264::parseNalUnit(data, size, state->nal)) return G2_H264_ERROR;
    switch (state->decoder.processNal(state->nal)) {
        case sub0h264::DecodeStatus::FrameDecoded:
            state->frameReady = true;
            return G2_H264_FRAME_READY;
        case sub0h264::DecodeStatus::NeedMoreData: return G2_H264_CONSUMED;
        default: return G2_H264_ERROR;
    }
}
