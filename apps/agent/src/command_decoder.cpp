#include "command_decoder.h"

#include "zfleet/protocol/control_codec.h"

#include <stdexcept>

namespace zfleet::agent {

std::vector<zfleet::protocol::ServerCommand> DecodeServerCommands(
    zfleet::transport::FrameDecoder* decoder,
    std::span<const std::uint8_t> bytes) {
  if (decoder == nullptr) {
    throw std::invalid_argument("command frame decoder must not be null");
  }

  const auto frames = decoder->Push(bytes);
  std::vector<zfleet::protocol::ServerCommand> commands;
  commands.reserve(frames.size());
  for (const auto& frame : frames) {
    commands.push_back(zfleet::protocol::DecodeServerCommandPayload(
        std::span<const std::uint8_t>{frame.data(), frame.size()}));
  }
  return commands;
}

} // namespace zfleet::agent
