#include "nxtrt/format.hpp"

namespace nxtrt {

std::string vformat(std::string_view fmt, std::format_args args)
{
    return std::vformat(fmt, args);
}

} // namespace nxtrt
