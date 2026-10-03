/** Sub0h264 — optional embedded allocation preflight support.
 *
 * Memory-constrained integrations can reserve a complete allocation batch
 * before std::vector mutates decoder state.  A failed reservation is reported
 * as a normal decoder error instead of reaching a platform allocator that may
 * trap on OOM.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef CROG_SUB0H264_ALLOCATION_PREFLIGHT_HPP
#define CROG_SUB0H264_ALLOCATION_PREFLIGHT_HPP

#include <cstddef>
#include <cstdint>

namespace sub0h264 {

enum class AllocationTag : uint32_t
{
    DpbEntries       = 1U,
    FrameY           = 2U,
    FrameU           = 3U,
    FrameV           = 4U,
    NnzLuma          = 5U,
    NnzCb            = 6U,
    NnzCr            = 7U,
    MbQps            = 8U,
    MbTransform8x8   = 9U,
    MbMotion         = 10U,
    MbIntra4x4Modes  = 11U,
    CabacNeighbors   = 12U,
};

struct AllocationRequest
{
    AllocationTag tag;
    size_t size;
};

struct AllocationFailure
{
    AllocationTag tag = static_cast<AllocationTag>(0U);
    size_t size = 0U;
};

using AllocationPreflightFn =
    bool (*)(const AllocationRequest* requests, size_t count,
             AllocationFailure* failure) noexcept;

} // namespace sub0h264

#if defined(SUB0H264_ALLOCATION_PREFLIGHT)
extern "C" bool SUB0H264_ALLOCATION_PREFLIGHT(
    const sub0h264::AllocationRequest* requests,
    size_t count,
    sub0h264::AllocationFailure* failure) noexcept;
#endif

namespace sub0h264 {

#if defined(SUB0H264_ALLOCATION_PREFLIGHT)

inline bool allocationPreflight(
    const AllocationRequest* requests,
    size_t count,
    AllocationFailure* failure = nullptr) noexcept
{
    return SUB0H264_ALLOCATION_PREFLIGHT(requests, count, failure);
}

#else

inline AllocationPreflightFn& allocationPreflightOverride() noexcept
{
    static AllocationPreflightFn callback = nullptr;
    return callback;
}

inline void setAllocationPreflightForTesting(
    AllocationPreflightFn callback) noexcept
{
    allocationPreflightOverride() = callback;
}

inline bool allocationPreflight(
    const AllocationRequest* requests,
    size_t count,
    AllocationFailure* failure = nullptr) noexcept
{
    AllocationPreflightFn callback = allocationPreflightOverride();
    return callback == nullptr || callback(requests, count, failure);
}

#endif

} // namespace sub0h264

#endif // CROG_SUB0H264_ALLOCATION_PREFLIGHT_HPP
