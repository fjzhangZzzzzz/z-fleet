#pragma once

#include "zfleet/protocol/message.h"
#include "zfleet/transport/frame_codec.h"

#include <cstdint>
#include <span>
#include <vector>

namespace zfleet::agent {

std::vector<zfleet::protocol::ServerCommand> DecodeServerCommands(
    zfleet::transport::FrameDecoder* decoder,
    std::span<const std::uint8_t> bytes);

} // namespace zfleet::agent
