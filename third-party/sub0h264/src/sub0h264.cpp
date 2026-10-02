/** Sub0h264 core implementation */
#include "sub0h264/sub0h264.hpp"
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
