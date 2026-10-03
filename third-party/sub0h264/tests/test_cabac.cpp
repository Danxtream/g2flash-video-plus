#include "doctest.h"
#include "../components/sub0h264/src/cabac.hpp"

using namespace sub0h264;

TEST_CASE("CABAC: CabacCtx state/mps extraction")
{
    CabacCtx ctx;
    ctx.mpsState = 0U;
    CHECK(ctx.state() == 0U);
    CHECK(ctx.mps() == 0U);

    ctx.mpsState = 0x40U; // bit 6 set = MPS=1, state=0
    CHECK(ctx.state() == 0U);
    CHECK(ctx.mps() == 1U);

    ctx.mpsState = 0x7FU; // all bits set
    CHECK(ctx.state() == 63U);
    CHECK(ctx.mps() == 1U);
}

TEST_CASE("CABAC: engine init reads 9 bits")
{
    // 9 bits of 0xFF, 0x80 → first 9 bits = 111111111 = 511
    const uint8_t data[] = { 0xFF, 0x80 };
    BitReader br(data, 2U);

    CabacEngine engine;
    engine.init(br);

    // After init, 9 bits consumed
    CHECK(br.bitOffset() == 9U);
}

TEST_CASE("CABAC: bypass bins decode")
{
    // Feed known bits and verify bypass decoding
    // Bypass mode: codIOffset = (codIOffset << 1) | nextBit
    //              if offset >= range: symbol=1, offset -= range
    //              else: symbol=0
    const uint8_t data[] = { 0x00, 0x00, 0xFF, 0xFF };
    BitReader br(data, 4U);

    CabacEngine engine;
    engine.init(br); // Reads 9 bits of zeros → offset = 0

    // With offset=0 and range=510, bypass should return 0 for low bits
    uint32_t b = engine.decodeBypass();
    CHECK(b == 0U); // offset still < range
}

TEST_CASE("CABAC: terminate bin")
{
    // Test the terminate bin behavior
    // After init with offset=0, range=510:
    // terminate: range -= 2 → 508, offset(0) < 508 → return 0 (not end)
    const uint8_t data[] = { 0x00, 0x00, 0x00, 0x00 };
    BitReader br(data, 4U);

    CabacEngine engine;
    engine.init(br);

    uint32_t term = engine.decodeTerminate();
    CHECK(term == 0U); // Not end of slice
}

TEST_CASE("CABAC: clz32 correctness")
{
    CHECK(clz32(0x80000000U) == 0U);
    CHECK(clz32(0x40000000U) == 1U);
    CHECK(clz32(0x00000001U) == 31U);
    CHECK(clz32(0x00000100U) == 23U);
    CHECK(clz32(256U) == 23U);
    CHECK(clz32(0U) == 32U);
}

TEST_CASE("CABAC: context init computes from m,n table")
{
    CabacCtx ctx[cNumCabacCtx];
    /// I-slice (sliceType=2), cabacInitIdc=0 (ignored for I), QP=26 (mid-range).
    static constexpr uint32_t cISliceType = 2U;
    static constexpr int32_t cMidRangeQp = 26;
    initCabacContexts(ctx, cISliceType, 0U, cMidRangeQp);

    // Verify contexts are non-trivially initialized (not all zero)
    uint32_t nonZeroCount = 0U;
    for (uint32_t i = 0U; i < cNumCabacCtx; ++i)
        if (ctx[i].mpsState != 0U) ++nonZeroCount;
    CHECK(nonZeroCount > 100U); // Most of 460 contexts should be non-zero

    // Spot-check: verify computeCabacInitState matches expected for known (m, n)
    // I-slice init_idc=3, context 1: m=2, n=54 → preCtxState = ((2*26)>>4)+54 = 3+54 = 57
    // preCtxState=57 <= 63 → pStateIdx = 63-57 = 6, valMPS = 0
    // mpsState = (6 & 0x3F) | (0 << 6) = 6
    CHECK(computeCabacInitState(2, 54, cMidRangeQp) == 6U);
}

TEST_CASE("CABAC: table has 128 entries with 4 ranges each")
{
    // Verify table dimensions and some known values
    CHECK(cCabacTable[0][0] == 2097536U);
    CHECK(cCabacTable[63][0] == 2080514U); // Last state=63 MPS=0
    CHECK(cCabacTable[127][0] == 4194050U); // Last state=63 MPS=1

    // rangeTabLPS is in bits 0-7
    uint32_t rangeLPS_state0_range0 = cCabacTable[0][0] & 0xFFU;
    CHECK(rangeLPS_state0_range0 == 128U); // 2097536 & 0xFF = 128
}
