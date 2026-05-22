#pragma once

#include "database.h"

#include "zfleet/protocol/v1/agent_control.pb.h"

#include <string>
#include <string_view>

namespace zfleet::server {

enum class ControlEventStatus {
  kAccepted,
  kInvalidArgument,
  kNotFound,
  kInternalError,
};

struct ControlEventResult {
  ControlEventStatus status;
  std::string message;
};

class Http2ControlService {
 public:
  explicit Http2ControlService(ServerDatabase* database);

  ControlEventResult HandleAgentEvent(
      const zfleet::protocol::v1::AgentEvent& event) const;

 private:
  ControlEventResult HandleRegister(
      const zfleet::protocol::v1::AgentEvent& event) const;
  ControlEventResult HandleHeartbeat(
      const zfleet::protocol::v1::AgentEvent& event) const;
  ControlEventResult ValidateEnvelope(
      const zfleet::protocol::v1::AgentEvent& event) const;
  void RecordAuditEvent(zfleet::protocol::AuditEventType event_type,
                        std::string request_id,
                        std::string agent_id,
                        std::string result,
                        std::string payload_json) const;

  ServerDatabase* database_;
};

std::string_view ToString(ControlEventStatus status) noexcept;

} // namespace zfleet::server
