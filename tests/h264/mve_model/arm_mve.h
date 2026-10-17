/* Host lane model for kernel algebra, NOT instruction/ABI emulation.
 * Real ARM compilation uses the toolchain's arm_mve.h, never this directory.
 * SPDX-License-Identifier: MIT */
#pragma once
#include <array>
#include <algorithm>
#include <cstdint>
#include <cstdlib>

using int32x4_t = std::array<int32_t,4>;
using uint32x4_t = std::array<uint32_t,4>;
using mve_pred16_t = uint16_t;
inline bool active(mve_pred16_t p, unsigned i) { return (p & (15U << (4*i))) != 0; }
inline mve_pred16_t vctp32q(uint32_t n) {
    mve_pred16_t p = 0;
    for (unsigned i=0;i<4;++i) if (i<n) p |= 15U << (4*i);
    return p;
}
inline int32x4_t vreinterpretq_s32_u32(uint32x4_t a) {
    int32x4_t r; for (unsigned i=0;i<4;++i) r[i]=static_cast<int32_t>(a[i]); return r;
}
inline uint32x4_t vreinterpretq_u32_s32(int32x4_t a) {
    uint32x4_t r; for (unsigned i=0;i<4;++i) r[i]=static_cast<uint32_t>(a[i]); return r;
}
inline int32x4_t vdupq_n_s32(int32_t a) { return {a,a,a,a}; }
inline uint32x4_t vidupq_n_u32(uint32_t a, unsigned step) { return {a,a+step,a+2*step,a+3*step}; }
inline int32x4_t vaddq_s32(int32x4_t a,int32x4_t b) {
    for (unsigned i=0;i<4;++i) a[i]+=b[i]; return a;
}
inline int32x4_t vsubq_s32(int32x4_t a,int32x4_t b) {
    for (unsigned i=0;i<4;++i) a[i]-=b[i]; return a;
}
inline int32x4_t vmulq_n_s32(int32x4_t a,int32_t n) {
    for (auto& v:a) v*=n; return a;
}
inline uint32x4_t vmulq_n_u32(uint32x4_t a,uint32_t n) {
    for (auto& v:a) v*=n; return a;
}
inline int32x4_t vaddq_n_s32(int32x4_t a,int32_t n) {
    for (auto& v:a) v+=n; return a;
}
inline int32x4_t vshrq_n_s32(int32x4_t a,int n) {
    for (auto& v:a) v>>=n; return a;
}
inline int32x4_t vmaxq_s32(int32x4_t a,int32x4_t b) {
    for (unsigned i=0;i<4;++i) a[i]=std::max(a[i],b[i]); return a;
}
inline int32x4_t vminq_s32(int32x4_t a,int32x4_t b) {
    for (unsigned i=0;i<4;++i) a[i]=std::min(a[i],b[i]); return a;
}
inline int32x4_t vnegq_s32(int32x4_t a) { for(auto& v:a) v=-v; return a; }
inline int32x4_t vrhaddq_s32(int32x4_t a,int32x4_t b) {
    for(unsigned i=0;i<4;++i) a[i]=(a[i]+b[i]+1)>>1; return a;
}
inline int32x4_t vabdq_s32(int32x4_t a,int32x4_t b) {
    for(unsigned i=0;i<4;++i) a[i]=std::abs(a[i]-b[i]); return a;
}
inline mve_pred16_t vcmpltq_n_s32(int32x4_t a,int32_t b) {
    mve_pred16_t p=0; for(unsigned i=0;i<4;++i) if(a[i]<b) p |= 15U << (4*i); return p;
}
inline int32x4_t vpselq_s32(int32x4_t a,int32x4_t b,mve_pred16_t p) {
    for(unsigned i=0;i<4;++i) if(!active(p,i)) a[i]=b[i]; return a;
}
inline uint32x4_t vldrbq_z_u32(const uint8_t* p,mve_pred16_t lanes) {
    uint32x4_t a{}; for(unsigned i=0;i<4;++i) if(active(lanes,i)) a[i]=p[i]; return a;
}
inline int32x4_t vldrhq_z_s32(const int16_t* p,mve_pred16_t lanes) {
    int32x4_t a{}; for(unsigned i=0;i<4;++i) if(active(lanes,i)) a[i]=p[i]; return a;
}
inline void vstrbq_p_u32(uint8_t* p,uint32x4_t a,mve_pred16_t lanes) {
    for(unsigned i=0;i<4;++i) if(active(lanes,i)) p[i]=static_cast<uint8_t>(a[i]);
}
inline void vstrhq_p_s32(int16_t* p,int32x4_t a,mve_pred16_t lanes) {
    for(unsigned i=0;i<4;++i) if(active(lanes,i)) p[i]=static_cast<int16_t>(a[i]);
}
inline uint32x4_t vldrbq_gather_offset_u32(const uint8_t* p,uint32x4_t offsets) {
    uint32x4_t a; for(unsigned i=0;i<4;++i) a[i]=p[offsets[i]]; return a;
}
inline void vstrbq_scatter_offset_u32(uint8_t* p,uint32x4_t offsets,uint32x4_t a) {
    for(unsigned i=0;i<4;++i) p[offsets[i]]=static_cast<uint8_t>(a[i]);
}
