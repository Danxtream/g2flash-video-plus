/** Optional reconstruction features preserve upstream defaults.
 *  SPDX-License-Identifier: MIT
 */
#ifndef CROG_SUB0H264_FEATURES_HPP
#define CROG_SUB0H264_FEATURES_HPP

/** Set to 0 to omit U/V reconstruction and frame storage. Chroma residual
 *  syntax and entropy-neighbor state remain necessary to decode Y correctly.
 *  When disabled, the runtime skipChroma setting is always effectively true.
 */
#ifndef SUB0H264_ENABLE_CHROMA_RECONSTRUCTION
#define SUB0H264_ENABLE_CHROMA_RECONSTRUCTION 1
#endif

#if SUB0H264_ENABLE_CHROMA_RECONSTRUCTION != 0 && SUB0H264_ENABLE_CHROMA_RECONSTRUCTION != 1
#error "SUB0H264_ENABLE_CHROMA_RECONSTRUCTION must be 0 or 1"
#endif

#endif // CROG_SUB0H264_FEATURES_HPP
