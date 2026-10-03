/** Sub0h264 — Unity build translation unit
 *
 *  Includes the entire decoder in a single translation unit so the
 *  compiler can fully inline and optimize across all modules. This is
 *  critical for embedded targets (ESP32-P4) where LTO may not be
 *  available and cross-TU inlining is essential for performance.
 *
 *  Build with: -O2 or -O3 for full optimization.
 *
 *  SPDX-License-Identifier: MIT
 */

// ── Public API ──────────────────────────────────────────────────────────
#include "sub0h264/sub0h264.hpp"

// ── Decoder pipeline (all headers included transitively via decoder.hpp)
#include "decoder.hpp"

// ── Platform abstraction ────────────────────────────────────────────────
#include "sub0h264_platform.hpp"

namespace sub0h264 {

const char* getVersionString() noexcept
{
    return "0.1.0";
}

Version getVersion() noexcept
{
    return Version{ 0U, 1U, 0U };
}

const char* platformName() noexcept
{
    return detail::platformName();
}

} // namespace sub0h264
