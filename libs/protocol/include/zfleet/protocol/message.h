#pragma once

#include <string_view>

namespace zfleet::protocol {

enum class MessageKind {
  registration,
  heartbeat,
  asset_snapshot,
};

std::string_view protocol_version() noexcept;
std::string_view to_string(MessageKind kind) noexcept;

} // namespace zfleet::protocol
