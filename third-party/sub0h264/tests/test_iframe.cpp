#include "doctest.h"
#include "../components/sub0h264/src/frame.hpp"
#include "../components/sub0h264/src/transform.hpp"
#include "../components/sub0h264/src/intra_pred.hpp"

using namespace sub0h264;

// ── Frame buffer tests ──────────────────────────────────────────────────

TEST_CASE("Frame: allocate 640x480")
{
    Frame frame;
    REQUIRE(frame.allocate(640U, 480U));
    CHECK(frame.width() == 640U);
    CHECK(frame.height() == 480U);
    CHECK(frame.yStride() == 640U);
    CHECK(frame.uvStride() == 320U);
    CHECK(frame.isAllocated());
}

TEST_CASE("Frame: pixel access")
{
    Frame frame;
    frame.allocate(16U, 16U);
    frame.fill(0U, 128U, 128U);

    CHECK(frame.y(0U, 0U) == 0U);
    CHECK(frame.u(0U, 0U) == 128U);

    frame.y(5U, 3U) = 200U;
    CHECK(frame.y(5U, 3U) == 200U);
}

TEST_CASE("Frame: macroblock access")
{
    Frame frame;
    frame.allocate(32U, 32U);
    frame.fill(0U, 0U, 0U);

    // MB (1,1) starts at pixel (16,16)
    uint8_t* mb = frame.yMb(1U, 1U);
    mb[0] = 255U;
    CHECK(frame.y(16U, 16U) == 255U);
}

// ── Inverse transform tests ─────────────────────────────────────────────

TEST_CASE("Transform: DC-only adds constant to prediction")
{
    uint8_t pred[16];
    uint8_t out[16];
    std::memset(pred, 100U, 16U);

    // DC coefficient that produces +10 after (dc + 32) >> 6
    // (dc + 32) >> 6 = 10 → dc = 10*64 - 32 = 608
    int16_t dc = 608;
    inverseDcOnly4x4AddPred(dc, pred, 4U, out, 4U);

    for (uint32_t i = 0U; i < 16U; ++i)
        CHECK(out[i] == 110U);
}

TEST_CASE("Transform: DC-only clips to 255")
{
    uint8_t pred[16];
    uint8_t out[16];
    std::memset(pred, 250U, 16U);

    int16_t dc = 640; // (640+32)>>6 = 10 → 250+10=260 → clipped to 255
    inverseDcOnly4x4AddPred(dc, pred, 4U, out, 4U);

    for (uint32_t i = 0U; i < 16U; ++i)
        CHECK(out[i] == 255U);
}

TEST_CASE("Transform: DC-only clips to 0")
{
    uint8_t pred[16];
    uint8_t out[16];
    std::memset(pred, 5U, 16U);

    int16_t dc = -640; // (-640+32)>>6 = -9 → 5-9=-4 → clipped to 0
    inverseDcOnly4x4AddPred(dc, pred, 4U, out, 4U);

    for (uint32_t i = 0U; i < 16U; ++i)
        CHECK(out[i] == 0U);
}

TEST_CASE("Transform: 4x4 inverse DCT with zero coefficients produces prediction")
{
    uint8_t pred[16];
    uint8_t out[16];
    int16_t coeffs[16] = {};
    std::memset(pred, 42U, 16U);

    inverseDct4x4AddPred(coeffs, pred, 4U, out, 4U);

    // Zero residual → output = prediction
    for (uint32_t i = 0U; i < 16U; ++i)
        CHECK(out[i] == 42U);
}

TEST_CASE("Transform: Hadamard 2x2 roundtrip")
{
    // Forward Hadamard: [a,b;c,d] → [a+b+c+d, a-b+c-d; a+b-c-d, a-b-c+d]
    // Inverse should recover (up to scaling)
    int16_t dc[4] = { 4, 0, 0, 0 };
    inverseHadamard2x2(dc);
    // [4+0+0+0, 4-0+0-0, 4+0-0-0, 4-0-0+0] = [4, 4, 4, 4]
    CHECK(dc[0] == 4);
    CHECK(dc[1] == 4);
    CHECK(dc[2] == 4);
    CHECK(dc[3] == 4);
}

TEST_CASE("Transform: Hadamard 4x4 DC block")
{
    // All-DC input: single value at [0,0]
    int16_t dc[16] = {};
    dc[0] = 64;
    inverseHadamard4x4(dc);
    // All outputs should be equal (DC spread to all positions)
    for (uint32_t i = 0U; i < 16U; ++i)
        CHECK(dc[i] == 64);
}

// ── Intra prediction tests ──────────────────────────────────────────────

TEST_CASE("IntraPred 4x4: Vertical copies top row")
{
    uint8_t top[4] = { 10, 20, 30, 40 };
    uint8_t pred[16];

    intraPred4x4(Intra4x4Mode::Vertical, top, nullptr, nullptr, nullptr, pred);

    for (uint32_t row = 0U; row < 4U; ++row)
    {
        CHECK(pred[row * 4 + 0] == 10U);
        CHECK(pred[row * 4 + 1] == 20U);
        CHECK(pred[row * 4 + 2] == 30U);
        CHECK(pred[row * 4 + 3] == 40U);
    }
}

TEST_CASE("IntraPred 4x4: Horizontal copies left column")
{
    uint8_t left[4] = { 100, 110, 120, 130 };
    uint8_t pred[16];

    intraPred4x4(Intra4x4Mode::Horizontal, nullptr, nullptr, left, nullptr, pred);

    for (uint32_t row = 0U; row < 4U; ++row)
        for (uint32_t col = 0U; col < 4U; ++col)
            CHECK(pred[row * 4 + col] == left[row]);
}

TEST_CASE("IntraPred 4x4: DC with both neighbors")
{
    uint8_t top[4]  = { 10, 10, 10, 10 };
    uint8_t left[4] = { 20, 20, 20, 20 };
    uint8_t pred[16];

    intraPred4x4(Intra4x4Mode::Dc, top, nullptr, left, nullptr, pred);

    // Average = (40 + 80 + 4) >> 3 = 15
    uint8_t expected = static_cast<uint8_t>((40U + 80U + 4U) >> 3U);
    for (uint32_t i = 0U; i < 16U; ++i)
        CHECK(pred[i] == expected);
}

TEST_CASE("IntraPred 4x4: DC with no neighbors")
{
    uint8_t pred[16];
    intraPred4x4(Intra4x4Mode::Dc, nullptr, nullptr, nullptr, nullptr, pred);

    for (uint32_t i = 0U; i < 16U; ++i)
        CHECK(pred[i] == cDefaultPredValue);
}

TEST_CASE("IntraPred 16x16: DC with full neighbors")
{
    Frame frame;
    frame.allocate(32U, 32U);
    // Set top row (y=15, x=16..31) to 100
    for (uint32_t x = 16U; x < 32U; ++x)
        frame.y(x, 15U) = 100U;
    // Set left column (x=15, y=16..31) to 200
    for (uint32_t y = 16U; y < 32U; ++y)
        frame.y(15U, y) = 200U;

    uint8_t pred[256];
    intraPred16x16(Intra16x16Mode::Dc, frame, 1U, 1U, pred);

    // DC = (16*100 + 16*200 + 16) >> 5 = (1600+3200+16)/32 = 150
    uint8_t expected = static_cast<uint8_t>((1600U + 3200U + 16U) >> 5U);
    CHECK(pred[0] == expected);
    CHECK(pred[255] == expected);
}

TEST_CASE("IntraPred chroma: DC 8x8")
{
    Frame frame;
    frame.allocate(16U, 16U);
    frame.fill(0U, 100U, 200U);

    uint8_t pred[64];
    intraPredChroma8x8(IntraChromaMode::Dc, frame, 0U, 0U, true, pred);

    // MB (0,0): no top, no left → DC = 128
    CHECK(pred[0] == cDefaultPredValue);
}

TEST_CASE("clipU8 boundaries")
{
    CHECK(clipU8(-10) == 0);
    CHECK(clipU8(0) == 0);
    CHECK(clipU8(128) == 128);
    CHECK(clipU8(255) == 255);
    CHECK(clipU8(300) == 255);
}
