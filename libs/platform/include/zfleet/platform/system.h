#pragma once

#include <string>
#include <string_view>

namespace zfleet::platform {

std::string_view os_name() noexcept;
std::string architecture_name();
std::string hostname();

} // namespace zfleet::platform
