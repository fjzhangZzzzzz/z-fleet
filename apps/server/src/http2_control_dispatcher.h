#pragma once

#include "http2_control_service.h"

#include "zfleet/transport/frame_codec.h"

#include <cstdint>
#include <span>
#include <vector>

namespace zfleet::server {

class Http2ControlDispatcher {
 public:
  explicit Http2ControlDispatcher(Http2ControlService* service);

  std::vector<ControlEventResult> PushEventBytes(
      std::span<const std::uint8_t> bytes);

 private:
  Http2ControlService* service_;
  zfleet::transport::FrameDecoder decoder_;
};

} // namespace zfleet::server
