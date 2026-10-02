/** Sub0h264 public type definitions
 *
 *  This file is part of Sub0h264. Original source at https://github.com/Crog/Sub0h264
 *
 *  MIT License
 *
 * Copyright (c) 2026 Craig Hutchinson <craig-sub0h264@crog.uk>
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *  SOFTWARE.
 */
#ifndef CROG_SUB0H264_TYPES_HPP
#define CROG_SUB0H264_TYPES_HPP

#include <cstdint>

namespace sub0h264 {

/** Semantic version triplet */
struct Version
{
    uint8_t major_;
    uint8_t minor_;
    uint8_t patch_;
};

/** Result codes for decoder operations */
enum class Result : int32_t
{
    Ok              =  0,
    ErrorInvalidParam = -1,
    ErrorUnsupported  = -2,
};

} // namespace sub0h264

#endif // CROG_SUB0H264_TYPES_HPP
