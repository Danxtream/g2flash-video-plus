#include "doctest.h"
#include "../components/sub0h264/src/deblock.hpp"

using namespace sub0h264;

TEST_CASE("Deblock: alpha/beta tables valid range")
{
    CHECK(cAlphaTable[0] == 0U);
    CHECK(cAlphaTable[51] == 255U);
    CHECK(cBetaTable[0] == 0U);
    CHECK(cBetaTable[51] == 18U);
}

TEST_CASE("Deblock: tc0 table structure")
{
    // BS=0 column always 0
    for (uint32_t i = 0U; i < 52U; ++i)
        CHECK(cTc0Table[i][0] == 0U);

    // High QP has larger tc0
    CHECK(cTc0Table[51][3] == 25U);
}

TEST_CASE("Deblock: computeBs returns 4 for intra at MB boundary")
{
    CHECK(computeBs(true, false, true, false, false, 0, 0, 0, 0, -1, 0) == 4U);
    CHECK(computeBs(false, true, true, false, false, 0, 0, 0, 0, 0, -1) == 4U);
}

TEST_CASE("Deblock: computeBs returns 3 for intra at internal edge")
{
    CHECK(computeBs(true, false, false, false, false, 0, 0, 0, 0, -1, 0) == 3U);
}

TEST_CASE("Deblock: computeBs returns 2 for non-zero coefficients")
{
    CHECK(computeBs(false, false, true, true, false, 0, 0, 0, 0, 0, 0) == 2U);
    CHECK(computeBs(false, false, false, false, true, 0, 0, 0, 0, 0, 0) == 2U);
}

TEST_CASE("Deblock: computeBs returns 1 for MV difference")
{
    // MV diff >= 4 quarter-pel (1 full pel)
    CHECK(computeBs(false, false, true, false, false, 0, 0, 4, 0, 0, 0) == 1U);
    CHECK(computeBs(false, false, true, false, false, 0, 0, 0, 4, 0, 0) == 1U);
}

TEST_CASE("Deblock: computeBs returns 0 for same inter conditions")
{
    CHECK(computeBs(false, false, true, false, false, 5, 3, 5, 3, 0, 0) == 0U);
}

TEST_CASE("Deblock: weak luma filter no-op below threshold")
{
    // When |p0-q0| >= alpha, no filtering occurs
    uint8_t p0 = 100U, p1 = 100U, p2 = 100U;
    uint8_t q0 = 200U, q1 = 200U, q2 = 200U;
    uint8_t origP0 = p0, origQ0 = q0;

    filterLumaWeak(p0, p1, p2, q0, q1, q2, 10, 10, 5);

    // |p0-q0| = 100 >= alpha=10, so no change
    CHECK(p0 == origP0);
    CHECK(q0 == origQ0);
}

TEST_CASE("Deblock: weak luma filter modifies edge pixels")
{
    // Smooth edge with small difference
    uint8_t p0 = 128U, p1 = 128U, p2 = 128U;
    uint8_t q0 = 132U, q1 = 132U, q2 = 132U;

    filterLumaWeak(p0, p1, p2, q0, q1, q2, 100, 100, 5);

    // Filter should smooth the 128→132 transition
    CHECK(p0 > 128U); // p0 moved towards q0
    CHECK(q0 < 132U); // q0 moved towards p0
}

TEST_CASE("Deblock: strong luma filter smooths intra boundary")
{
    uint8_t p0 = 128U, p1 = 128U, p2 = 128U, p3 = 128U;
    uint8_t q0 = 130U, q1 = 130U, q2 = 130U, q3 = 130U;

    filterLumaStrong(p0, p1, p2, q0, q1, q2, p3, q3, 255, 18);

    // Should smooth transition
    CHECK(p0 >= 128U);
    CHECK(q0 <= 130U);
}

TEST_CASE("Deblock: chroma weak filter")
{
    uint8_t p0 = 128U, p1 = 128U;
    uint8_t q0 = 132U, q1 = 132U;

    filterChromaWeak(p0, p1, q0, q1, 100, 100, 5);

    CHECK(p0 > 128U);
    CHECK(q0 < 132U);
}

// ── Per-MB QP and boundary averaging tests — ITU-T H.264 §8.7.2.2 ──────

TEST_CASE("Deblock: QP averaging at MB boundary produces different thresholds")
{
    // When two adjacent MBs have different QPs, the boundary edge should
    // use (qpP + qpQ + 1) >> 1 per ITU-T H.264 §8.7.2.2.
    // QP=16: alpha=cAlphaTable[16]=4
    // QP=24: alpha=cAlphaTable[24]=13
    // QP_avg = (16+24+1)>>1 = 20: alpha=cAlphaTable[20]=7
    static constexpr int32_t cQpP = 16;
    static constexpr int32_t cQpQ = 24;
    static constexpr int32_t cQpAvg = (cQpP + cQpQ + 1) >> 1; // = 20
    static_assert(cQpAvg == 20, "average QP must be 20");
    CHECK(cAlphaTable[cQpAvg] == 7U);
    CHECK(cAlphaTable[cQpP]   == 4U);
    CHECK(cAlphaTable[cQpQ]   == 12U);  // cAlphaTable[24]=12
    // The averaged index must differ from either single-MB index.
    CHECK(cAlphaTable[cQpAvg] != cAlphaTable[cQpP]);
    CHECK(cAlphaTable[cQpAvg] != cAlphaTable[cQpQ]);
}

TEST_CASE("Deblock: weak filter delta is bounded by tc")
{
    // With BS=1, QP=24: indexA=24, tc0=cTc0Table[24][1]=1.
    // |p0-q0|=4 < alpha=13, |p1-p0|=1 < beta=4, |q1-q0|=1 < beta=4.
    // ap=1 < beta → tc += 1; aq=1 < beta → tc += 1 → tc=3.
    // delta = (((4*4) + (0) + 4) >> 3) = (20>>3) = 2, clamped to ±3 → 2.
    // p0_new = 100+2 = 102; q0_new = 104-2 = 102.
    uint8_t p0 = 100U, p1 = 101U, p2 = 101U;
    uint8_t q0 = 104U, q1 = 103U, q2 = 103U;

    static constexpr int32_t cTestQp = 24;
    static constexpr int32_t cTc0  = 1; // cTc0Table[24][1]
    CHECK(cTc0Table[cTestQp][1] == cTc0);

    filterLumaWeak(p0, p1, p2, q0, q1, q2,
                   cAlphaTable[cTestQp], cBetaTable[cTestQp], cTc0);

    CHECK(p0 == 102U);
    CHECK(q0 == 102U);
}

TEST_CASE("Deblock: strong filter BS=4 averages 4 pixels on each side")
{
    // Verify the exact output of filterLumaStrong for known inputs.
    // §8.7.2.4: with strongThresh and ap < beta:
    //   p0 = (p2 + 2*p1 + 2*p0 + 2*q0 + q1 + 4) >> 3
    //      = (80 + 160 + 160 + 160 + 80 + 4) >> 3 = 644 >> 3 = 80
    //   q0 = (p1 + 2*p0 + 2*q0 + 2*q1 + q2 + 4) >> 3
    //      = (80 + 160 + 160 + 160 + 80 + 4) >> 3 = 644 >> 3 = 80
    uint8_t p0 = 80U, p1 = 80U, p2 = 80U, p3 = 80U;
    uint8_t q0 = 80U, q1 = 80U, q2 = 80U, q3 = 80U;

    // All-same-value: filter must be idempotent
    filterLumaStrong(p0, p1, p2, q0, q1, q2, p3, q3, 255, 18);
    CHECK(p0 == 80U);
    CHECK(q0 == 80U);
}

TEST_CASE("Deblock: strong filter threshold condition §8.7.2.4")
{
    // Strong filter only applies when |p0-q0| < (alpha >> 2) + 2.
    // With alpha=28 (QP=31): threshold = (28>>2)+2 = 9.
    // |p0-q0|=10 >= 9 → strongThresh=false → use weak-strong path:
    //   p0 = (2*p1 + p0 + q1 + 2) >> 2
    //   q0 = (2*q1 + q0 + p1 + 2) >> 2
    static constexpr int32_t cAlpha31 = 28;  // cAlphaTable[31]
    static constexpr int32_t cBeta31  = 8;   // cBetaTable[31]
    CHECK(cAlphaTable[31] == cAlpha31);
    CHECK(cBetaTable[31]  == cBeta31);

    uint8_t p0 = 100U, p1 = 100U, p2 = 100U, p3 = 100U;
    uint8_t q0 = 110U, q1 = 110U, q2 = 110U, q3 = 110U;
    // |p0-q0|=10 >= (28>>2)+2=9 → not strongThresh
    // → p0 = (200 + 100 + 110 + 2) >> 2 = 412>>2 = 103
    // → q0 = (220 + 110 + 100 + 2) >> 2 = 432>>2 = 108
    filterLumaStrong(p0, p1, p2, q0, q1, q2, p3, q3, cAlpha31, cBeta31);
    CHECK(p0 == 103U);
    CHECK(q0 == 108U);
}

// ── Spec table spot-checks — ITU-T H.264 Table 8-16 ─────────────────────

TEST_CASE("Deblock: alpha/beta table spec values at QP=24 per Table 8-16")
{
    // ITU-T H.264 Table 8-16: indexA/indexB → alpha/beta thresholds.
    // Spot-check at QP=24 (no offset), which is the QP of the baseline IDR.
    // cAlphaTable[24]=12, cBetaTable[24]=4 per spec.
    static constexpr int32_t cQp = 24;
    CHECK(cAlphaTable[cQp] == 12U);  // Table 8-16: alpha at QP=24
    CHECK(cBetaTable[cQp]  ==  4U);  // Table 8-16: beta at QP=24

    // tc0 at QP=24: cTc0Table[24] = {0, 1, 1, 1} per Table 8-16
    CHECK(cTc0Table[cQp][1] == 1U);  // BS=1
    CHECK(cTc0Table[cQp][2] == 1U);  // BS=2
    CHECK(cTc0Table[cQp][3] == 1U);  // BS=3
}

TEST_CASE("Deblock: tc calculation for BS=3 QP=24 with smooth neighbors §8.7.2.3")
{
    // ITU-T H.264 §8.7.2.3: tc = tc0 + (ap < beta ? 1 : 0) + (aq < beta ? 1 : 0).
    // QP=24: alpha=12, beta=4, tc0=cTc0Table[24][3]=1.
    // With smooth neighbors (ap=aq=0 < beta=4): tc = 1 + 1 + 1 = 3.
    // Inputs: p0=80, p1=81, p2=81; q0=84, q1=81, q2=81.
    // |p0-q0|=4 < alpha=12 → filter applies.
    // delta = (4*(84-80) + (81-81) + 4) >> 3 = (16+0+4)>>3 = 20>>3 = 2.
    // delta clamped to ±3: delta=2 (no clamp needed).
    // p0_new = 80+2=82; q0_new = 84-2=82.
    uint8_t p0 = 80U, p1 = 81U, p2 = 81U;
    uint8_t q0 = 84U, q1 = 81U, q2 = 81U;
    static constexpr int32_t cAlpha = 12;
    static constexpr int32_t cBeta  =  4;
    static constexpr int32_t cTc0   =  1;  // BS=3
    CHECK(cAlphaTable[24] == static_cast<uint8_t>(cAlpha));
    CHECK(cBetaTable[24]  == static_cast<uint8_t>(cBeta));
    CHECK(cTc0Table[24][3] == static_cast<uint8_t>(cTc0));
    filterLumaWeak(p0, p1, p2, q0, q1, q2, cAlpha, cBeta, cTc0);
    CHECK(p0 == 82U);
    CHECK(q0 == 82U);
}

TEST_CASE("Deblock: weak filter skips when |p0-q0| >= alpha §8.7.2.3")
{
    // §8.7.2.3 decision: if |p0-q0| >= alpha, no filtering.
    // QP=24: alpha=12. Set |p0-q0|=12 (exactly at boundary → no filter).
    uint8_t p0 = 80U, p1 = 81U, p2 = 81U;
    uint8_t q0 = 92U, q1 = 91U, q2 = 91U;  // |p0-q0|=12 = alpha
    filterLumaWeak(p0, p1, p2, q0, q1, q2, 12, 4, 1);
    CHECK(p0 == 80U);  // unchanged: |p0-q0|=12 is NOT < alpha=12
    CHECK(q0 == 92U);
}

TEST_CASE("Deblock: weak filter skips when |p1-p0| >= beta §8.7.2.3")
{
    // §8.7.2.3: if |p1-p0| >= beta, no filtering.
    // QP=24: beta=4. Set |p1-p0|=4 (at boundary → no filter).
    uint8_t p0 = 80U, p1 = 76U, p2 = 76U;  // |p1-p0|=4 = beta
    uint8_t q0 = 82U, q1 = 82U, q2 = 82U;  // |p0-q0|=2 < alpha=12
    filterLumaWeak(p0, p1, p2, q0, q1, q2, 12, 4, 1);
    CHECK(p0 == 80U);  // unchanged: |p1-p0|=4 is NOT < beta=4
    CHECK(q0 == 82U);
}

TEST_CASE("Deblock: intra MB boundary uses BS=4 strong filter §8.7.2.4")
{
    // §8.7.2.4: BS=4 uses strong filter (p0/q0 averaged with 3 neighbors).
    // With alpha=12, beta=4, |p0-q0|=4 < (alpha>>2)+2=5 → strongThresh=true.
    // Strong filter: p0 = (p2+2*p1+2*p0+2*q0+q1+4)>>3
    //                   = (81+162+160+168+81+4)>>3 = 656>>3 = 82
    //              q0 = (p1+2*p0+2*q0+2*q1+q2+4)>>3
    //                   = (81+160+168+166+83+4)>>3 = 662>>3 = 82 (integer)
    uint8_t p0 = 80U, p1 = 81U, p2 = 81U, p3 = 81U;
    uint8_t q0 = 84U, q1 = 83U, q2 = 83U, q3 = 83U;
    static constexpr int32_t cAlpha = 12;
    static constexpr int32_t cBeta  =  4;
    // |p0-q0|=4 < (12>>2)+2=5 → strong path (ap/aq determine p1/q1 update)
    filterLumaStrong(p0, p1, p2, q0, q1, q2, p3, q3, cAlpha, cBeta);
    // Verify filter applied (values changed from initial)
    CHECK(p0 != 80U);
    CHECK(q0 != 84U);
}
