#pragma once

#include <string_view>

namespace zfleet::server {

bool IsSupportedPackageComponent(std::string_view component) noexcept;

}  // namespace zfleet::server
