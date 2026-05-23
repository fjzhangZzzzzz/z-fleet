#include "command_decoder.h"

#include <stdexcept>

namespace zfleet::agent {

std::vector<zfleet::protocol::v1::ServerCommand> DecodeServerCommands(
    zfleet::transport::FrameDecoder* decoder,
    std::span<const std::uint8_t> bytes) {
  if (decoder == nullptr) {
    throw std::invalid_argument("command frame decoder must not be null");
  }

  const auto frames = decoder->Push(bytes);
  std::vector<zfleet::protocol::v1::ServerCommand> commands;
  commands.reserve(frames.size());
  for (const auto& frame : frames) {
    zfleet::protocol::v1::ServerCommand command;
    if (!command.ParseFromArray(frame.data(), static_cast<int>(frame.size()))) {
      throw std::runtime_error("failed to parse server command");
    }
    commands.push_back(std::move(command));
  }
  return commands;
}

} // namespace zfleet::agent
