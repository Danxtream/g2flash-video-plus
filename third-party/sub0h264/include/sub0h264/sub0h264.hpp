/** Sub0h264 — H.264 Baseline+High decoder for ESP32-P4 and desktop
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
#ifndef CROG_SUB0H264_HPP
#define CROG_SUB0H264_HPP

#include "sub0h264_types.hpp"

namespace sub0h264 {

/** @return Semantic version string, e.g. "0.1.0" */
const char* getVersionString() noexcept;

/** @return Semantic version as a struct */
Version getVersion() noexcept;

/** @return Compile-time platform name: "generic", "x86", or "riscv_pie" */
const char* platformName() noexcept;

} // namespace sub0h264

#endif // CROG_SUB0H264_HPP
