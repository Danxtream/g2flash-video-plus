#include "doctest.h"

#include "../components/sub0h264/src/dpb.hpp"

using sub0h264::Dpb;
using sub0h264::Frame;

TEST_CASE("DPB allocates frame storage lazily and reuses it after flush")
{
    Dpb dpb;
    dpb.init(128U, 128U, 3U);

    CHECK(dpb.frameCapacity() == 4U);
    CHECK(dpb.allocatedFrameCount() == 0U);
    CHECK(dpb.allocatedFrameBytes() == 0U);

    Frame* first = dpb.getDecodeTarget(0U, 16U);
    REQUIRE(first != nullptr);
    CHECK(first->isAllocated());
    CHECK(first->width() == 128U);
    CHECK(first->height() == 128U);
    CHECK(dpb.allocatedFrameCount() == 1U);
    CHECK(dpb.allocatedFrameBytes() == 24576U);

    dpb.flush();

    Frame* reused = dpb.getDecodeTarget(0U, 16U);
    REQUIRE(reused != nullptr);
    CHECK(reused == first);
    CHECK(dpb.allocatedFrameCount() == 1U);
    CHECK(dpb.allocatedFrameBytes() == 24576U);
}

TEST_CASE("DPB allocates an additional frame only for a simultaneous target")
{
    Dpb dpb;
    dpb.init(128U, 128U, 3U);

    Frame* reference = dpb.getDecodeTarget(0U, 16U);
    REQUIRE(reference != nullptr);
    dpb.markAsReference(0U);

    Frame* current = dpb.getDecodeTarget(0U, 16U);
    REQUIRE(current != nullptr);
    CHECK(current != reference);
    CHECK(dpb.frameCapacity() == 4U);
    CHECK(dpb.allocatedFrameCount() == 2U);
    CHECK(dpb.allocatedFrameBytes() == 49152U);
}
