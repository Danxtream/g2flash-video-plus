/** Internal platform abstraction interface — not part of public API */
#ifndef CROG_SUB0H264_PLATFORM_HPP
#define CROG_SUB0H264_PLATFORM_HPP

namespace sub0h264 {
namespace detail {

/** @return Platform identifier string. Implemented per-platform. */
const char* platformName() noexcept;

} // namespace detail
} // namespace sub0h264

#endif // CROG_SUB0H264_PLATFORM_HPP
