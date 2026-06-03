#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "control_connection_registry.h"
#include "control_service.h"
#include "zfleet/transport/frame_codec.h"

namespace zfleet::server {

class ControlDispatcher {
 public:
  explicit ControlDispatcher(const ControlService* service);
  ControlDispatcher(const ControlService* service,
                    ControlConnectionRegistry* registry,
                    std::string connection_id);

  std::vector<ControlEventResult> PushEventBytes(
      std::span<const std::uint8_t> bytes);

 private:
  void RecordAcceptedEvent(const zfleet::protocol::AgentEvent& event) const;

  const ControlService* service_;
  ControlConnectionRegistry* registry_;
  std::string connection_id_;
  zfleet::transport::FrameDecoder decoder_;
};

}  // namespace zfleet::server
