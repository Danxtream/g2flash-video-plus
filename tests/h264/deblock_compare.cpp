/* Scalar oracle comparisons of edge kernels and complete MB traversal.
 * SPDX-License-Identifier: MIT */
#include "deblock.hpp"
#include "deblock_oracle.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <random>

static unsigned comparisons = 0;
#if defined(__ARM_FEATURE_MVE) && (__ARM_FEATURE_MVE & 1)
static void edge(const std::array<uint8_t,32>& samples, unsigned bs, unsigned qp, bool vertical)
{
    std::array<uint8_t,256> expected, actual;
    expected.fill(0xa5);
    unsigned across = vertical ? 1 : 16, along = vertical ? 16 : 1;
    unsigned origin = 4 * across;
    for (unsigned lane=0; lane<4; ++lane)
        for (unsigned tap=0; tap<8; ++tap)
            expected[origin + lane*along + tap*across - 4*across] = samples[lane*8+tap];
    actual = expected;
    int alpha = sub0h264::cAlphaTable[qp], beta = sub0h264::cBetaTable[qp];
    for (unsigned lane=0; lane<4; ++lane)
    {
        auto* q = expected.data()+origin+lane*along;
        if (bs==4)
            sub0h264::filterLumaStrong(q[-int(across)],q[-2*int(across)],q[-3*int(across)],
                q[0],q[across],q[2*across],q[-4*int(across)],q[3*across],alpha,beta);
        else if (bs)
            sub0h264::filterLumaWeak(q[-int(across)],q[-2*int(across)],q[-3*int(across)],
                q[0],q[across],q[2*across],alpha,beta,sub0h264::cTc0Table[qp][bs]);
    }
    if (bs==4) sub0h264::filterLumaMveStrong(actual.data()+origin,across,along,alpha,beta);
    else if (bs) sub0h264::filterLumaMveWeak(actual.data()+origin,across,along,alpha,beta,sub0h264::cTc0Table[qp][bs]);
    if(actual!=expected) { std::fprintf(stderr,"Deblock mismatch bs=%u qp=%u vertical=%u\n",bs,qp,vertical); std::abort(); }
    ++comparisons;
}
#endif

int main()
{
    std::mt19937 random(0xbb67ae85);
#if defined(__ARM_FEATURE_MVE) && (__ARM_FEATURE_MVE & 1)
    std::array<uint8_t,32> samples;
    // Exhaust every p0/q0 byte pair, all strengths and both orientations.
    // Other lanes straddle threshold and clipping boundaries independently.
    for (unsigned p=0;p<256;++p) for(unsigned q=0;q<256;++q)
    {
        for(unsigned lane=0;lane<4;++lane) for(unsigned tap=0;tap<8;++tap)
            samples[lane*8+tap]=static_cast<uint8_t>(std::clamp(int(tap<4?p:q)+int(lane)*(int(tap)-3),0,255));
        for(unsigned bs=0;bs<=4;++bs) for(bool vertical:{false,true}) edge(samples,bs,51,vertical);
    }
    for(unsigned qp=0;qp<52;++qp) for(unsigned bs=0;bs<=4;++bs)
        for(unsigned n=0;n<100;++n)
        {
            int centre=int(random()%256);
            for(auto& p:samples) p=static_cast<uint8_t>(n&1 ? random() : std::clamp(centre+int(random()%41)-20,0,255));
            edge(samples,bs,qp,false); edge(samples,bs,qp,true);
        }
#endif
    for(unsigned n=0;n<1000;++n)
    {
        sub0h264::Frame expected, actual;
        if(!expected.allocate(32,32,true) || !actual.allocate(32,32,true)) return 2;
        for(unsigned y=0;y<32;++y) for(unsigned x=0;x<32;++x)
            expected.y(x,y)=actual.y(x,y)=static_cast<uint8_t>(n&1 ? random() : 120+int(random()%31)-15);
        std::array<uint8_t,64> nnz{};
        std::array<sub0h264::MbMotionInfo,64> motion{};
        std::array<int32_t,4> qps{};
        std::array<uint8_t,4> tx8{};
        for(unsigned i=0;i<64;++i)
        {
            nnz[i]=n%3==0 ? 0 : random()%3;
            motion[i].refIdx=n%3==0 ? 0 : int(random()%2)-1;
            motion[i].mv={static_cast<int16_t>(n%3==0?0:int(random()%17)-8),0};
        }
        for(unsigned i=0;i<4;++i) { qps[i]=random()%52; tx8[i]=random()%2; }
        if(n==0) { qps={51,0,0,0}; }
        int ao=int(random()%25)-12, bo=int(random()%25)-12;
        for(unsigned my=0;my<2;++my) for(unsigned mx=0;mx<2;++mx)
        {
            bool intra=motion[(my*2+mx)*16].refIdx==-1;
            sub0h264::referenceDeblockMb(expected,mx,my,intra,ao,bo,nnz.data(),motion.data(),qps.data(),tx8.data(),0,2,2,true);
            sub0h264::deblockMb(actual,mx,my,intra,ao,bo,nnz.data(),motion.data(),qps.data(),tx8.data(),0,2,2,true);
        }
        for(unsigned y=0;y<32;++y) for(unsigned x=0;x<32;++x)
            if(expected.y(x,y)!=actual.y(x,y)) std::abort();
        ++comparisons;
    }
    std::printf("Deblock original C oracle: %u comparisons PASS\n",comparisons);
}
