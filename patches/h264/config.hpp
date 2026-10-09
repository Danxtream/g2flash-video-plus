/* Decoder configuration for the firmware.
 * SPDX-License-Identifier: GPL-3.0-only */
#pragma once

/* Printf/entropy trace is off; structured DecodeTrace callbacks remain compiled.
 * Parameter-set IDs are zero in the G2 stream. The DPB owns the only output. */
#define SUB0H264_TRACE 0
#define SUB0H264_ENABLE_CHROMA_RECONSTRUCTION 0
#define SUB0H264_MAX_SPS_COUNT 1U
#define SUB0H264_MAX_PPS_COUNT 1U
#define SUB0H264_DISABLE_LEGACY_CURRENT_FRAME 1
#define SUB0H264_ALLOCATION_PREFLIGHT g2_h264_preflight

#include "timing.hpp"
