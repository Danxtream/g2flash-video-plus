#include "doctest.h"

#include "../components/sub0h264/src/allocation_preflight.hpp"
#include "../components/sub0h264/src/dpb.hpp"
#include "../components/sub0h264/src/frame.hpp"

#include <cstddef>

namespace {

sub0h264::AllocationTag gFailTag = sub0h264::AllocationTag::DpbEntries;
sub0h264::AllocationRequest gLastRequests[8]{};
size_t gLastRequestCount = 0U;

bool recordingPreflight(
    const sub0h264::AllocationRequest* requests,
    size_t count,
    sub0h264::AllocationFailure* failure) noexcept
{
    gLastRequestCount = count;
    for (size_t i = 0U; i < count && i < 8U; ++i)
        gLastRequests[i] = requests[i];

    for (size_t i = 0U; i < count; ++i)
        if (requests[i].tag == gFailTag) {
            if (failure != nullptr) {
                failure->tag = requests[i].tag;
                failure->size = requests[i].size;
            }
            return false;
        }
    return true;
}

struct PreflightReset
{
    ~PreflightReset()
    {
        sub0h264::setAllocationPreflightForTesting(nullptr);
    }
};

} // namespace

TEST_CASE("allocation preflight rejects a complete 320x192 frame batch safely")
{
    PreflightReset reset;
    gFailTag = sub0h264::AllocationTag::FrameU;
    gLastRequestCount = 0U;
    sub0h264::setAllocationPreflightForTesting(recordingPreflight);

    sub0h264::Frame frame;
    CHECK_FALSE(frame.allocate(320U, 192U));
    CHECK_FALSE(frame.isAllocated());
    CHECK(frame.width() == 0U);
    REQUIRE(gLastRequestCount == 3U);
    CHECK(gLastRequests[0].tag == sub0h264::AllocationTag::FrameY);
    CHECK(gLastRequests[0].size == 61440U);
    CHECK(gLastRequests[1].tag == sub0h264::AllocationTag::FrameU);
    CHECK(gLastRequests[1].size == 15360U);
    CHECK(gLastRequests[2].tag == sub0h264::AllocationTag::FrameV);
    CHECK(gLastRequests[2].size == 15360U);
}

TEST_CASE("allocation preflight rejects DPB metadata without mutating capacity")
{
    PreflightReset reset;
    gFailTag = sub0h264::AllocationTag::DpbEntries;
    gLastRequestCount = 0U;
    sub0h264::setAllocationPreflightForTesting(recordingPreflight);

    sub0h264::Dpb dpb;
    CHECK_FALSE(dpb.init(320U, 192U, 1U));
    CHECK(dpb.frameCapacity() == 0U);
    REQUIRE(gLastRequestCount == 1U);
    CHECK(gLastRequests[0].tag == sub0h264::AllocationTag::DpbEntries);
    CHECK(gLastRequests[0].size > 0U);
}

TEST_CASE("allocation preflight success preserves normal frame allocation")
{
    PreflightReset reset;
    gFailTag = static_cast<sub0h264::AllocationTag>(0xffffffffU);
    sub0h264::setAllocationPreflightForTesting(recordingPreflight);

    sub0h264::Frame frame;
    CHECK(frame.allocate(320U, 192U));
    CHECK(frame.isAllocated());
    CHECK(frame.width() == 320U);
    CHECK(frame.height() == 192U);
}
