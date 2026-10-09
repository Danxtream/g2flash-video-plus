/** Default feature builds must preserve color and runtime reconstruction choice.
 *  SPDX-License-Identifier: MIT
 */
#include "doctest.h"
#include "../components/sub0h264/src/decoder.hpp"
#include <memory>

TEST_CASE("Build features: chroma reconstruction is enabled by default")
{
    static_assert(SUB0H264_ENABLE_CHROMA_RECONSTRUCTION == 1);
    auto color = std::make_unique<sub0h264::H264Decoder>();
    auto luma = std::make_unique<sub0h264::H264Decoder>(
        sub0h264::H264Decoder::Settings{true});
    CHECK_FALSE(color->skipChroma());
    CHECK(luma->skipChroma());
    sub0h264::Frame frame;
    REQUIRE(frame.allocate(16U, 16U));
    CHECK(frame.hasChroma());
    REQUIRE(frame.allocate(16U, 16U, true));
    CHECK_FALSE(frame.hasChroma());
}
