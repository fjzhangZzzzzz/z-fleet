#include "zfleet/protocol/message.h"

namespace zfleet::protocol {

std::string_view protocol_version() noexcept {
  return "v1";
}

std::string_view to_string(MessageKind kind) noexcept {
  switch (kind) {
    case MessageKind::registration:
      return "registration";
    case MessageKind::heartbeat:
      return "heartbeat";
    case MessageKind::asset_snapshot:
      return "asset_snapshot";
  }

  return "unknown";
}

} // namespace zfleet::protocol
