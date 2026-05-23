#pragma once

#include <cstdint>
#include <string_view>

namespace zfleet::transport {

std::uint32_t LinkedNghttp2Version();
std::string_view Http2ErrorMessage(std::uint32_t error_code) noexcept;

} // namespace zfleet::transport
