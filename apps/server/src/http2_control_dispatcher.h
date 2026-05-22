#pragma once

#include "http2_connection_registry.h"
#include "http2_control_service.h"

#include "zfleet/transport/frame_codec.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace zfleet::server {

class Http2ControlDispatcher {
 public:
  explicit Http2ControlDispatcher(const Http2ControlService* service);
  Http2ControlDispatcher(const Http2ControlService* service,
                         Http2ConnectionRegistry* registry,
                         std::string connection_id);

  std::vector<ControlEventResult> PushEventBytes(
      std::span<const std::uint8_t> bytes);

 private:
  void RecordAcceptedEvent(
      const zfleet::protocol::v1::AgentEvent& event) const;

  const Http2ControlService* service_;
  Http2ConnectionRegistry* registry_;
  std::string connection_id_;
  zfleet::transport::FrameDecoder decoder_;
};

} // namespace zfleet::server
