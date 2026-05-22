#include "http2_control_service.h"

#include "zfleet/core/log.h"
#include "zfleet/core/time.h"
#include "zfleet/core/uuid.h"
#include "zfleet/protocol/json_codec.h"

#include <cstdint>
#include <exception>
#include <utility>

namespace zfleet::server {
namespace {

namespace proto = zfleet::protocol::v1;

zfleet::core::log::Context EventLogger(std::string_view route,
                                       const proto::AgentEvent& event) {
  return zfleet::core::log::Component("server")
      .With({{"route", route},
             {"request_id", event.message_id()},
             {"agent_id", event.agent_id()}});
}

ControlEventResult Accepted(std::string message) {
  return ControlEventResult{
      .status = ControlEventStatus::kAccepted,
      .message = std::move(message),
  };
}

ControlEventResult InvalidArgument(std::string message) {
  return ControlEventResult{
      .status = ControlEventStatus::kInvalidArgument,
      .message = std::move(message),
  };
}

ControlEventResult NotFound(std::string message) {
  return ControlEventResult{
      .status = ControlEventStatus::kNotFound,
      .message = std::move(message),
  };
}

ControlEventResult InternalError() {
  return ControlEventResult{
      .status = ControlEventStatus::kInternalError,
      .message = "internal error",
  };
}

} // namespace

Http2ControlService::Http2ControlService(ServerDatabase* database)
    : database_(database) {}

ControlEventResult Http2ControlService::HandleAgentEvent(
    const proto::AgentEvent& event) const {
  const auto validation = ValidateEnvelope(event);
  if (validation.status != ControlEventStatus::kAccepted) {
    return validation;
  }

  switch (event.payload_case()) {
    case proto::AgentEvent::kRegister:
      return HandleRegister(event);
    case proto::AgentEvent::kHeartbeat:
      return HandleHeartbeat(event);
    case proto::AgentEvent::PAYLOAD_NOT_SET:
      return InvalidArgument("event payload must be set");
    default:
      return InvalidArgument("unsupported agent event payload");
  }
}

ControlEventResult Http2ControlService::ValidateEnvelope(
    const proto::AgentEvent& event) const {
  if (event.protocol_version() != zfleet::protocol::protocol_version()) {
    return InvalidArgument("unsupported protocol version");
  }
  if (event.message_id().empty()) {
    return InvalidArgument("message_id must not be empty");
  }
  if (event.agent_id().empty()) {
    return InvalidArgument("agent_id must not be empty");
  }
  if (event.occurred_at().empty()) {
    return InvalidArgument("occurred_at must not be empty");
  }
  return Accepted("accepted");
}

ControlEventResult Http2ControlService::HandleRegister(
    const proto::AgentEvent& event) const {
  const auto& registration = event.register_();
  const zfleet::protocol::RegistrationRequest request{
      .protocol_version = event.protocol_version(),
      .request_id = event.message_id(),
      .agent_id = event.agent_id(),
      .occurred_at = event.occurred_at(),
      .agent_version = registration.agent_version(),
      .hostname = registration.hostname(),
      .os = registration.os(),
      .arch = registration.arch(),
  };

  try {
    database_->UpsertAgent(request);
    RecordAuditEvent(
        zfleet::protocol::AuditEventType::agent_register, request.request_id,
        request.agent_id, "success",
        zfleet::protocol::SerializeAuditPayload(
            {{"transport", std::string("http2")},
             {"status", std::string("accepted")},
             {"agent_version", request.agent_version},
             {"hostname", request.hostname},
             {"os", request.os},
             {"arch", request.arch}}));
    ZFLOG_INFO(EventLogger("http2.control.register", event),
               "registration accepted");
    return Accepted("accepted");
  } catch (const std::exception& ex) {
    ZFLOG_ERROR(EventLogger("http2.control.register", event),
                "register failed: {}",
                ex.what());
    return InternalError();
  }
}

ControlEventResult Http2ControlService::HandleHeartbeat(
    const proto::AgentEvent& event) const {
  const auto& heartbeat = event.heartbeat();
  const zfleet::protocol::HeartbeatRequest request{
      .protocol_version = event.protocol_version(),
      .request_id = event.message_id(),
      .agent_id = event.agent_id(),
      .occurred_at = event.occurred_at(),
      .agent_version = heartbeat.agent_version(),
  };

  try {
    if (!database_->AgentExists(request.agent_id)) {
      RecordAuditEvent(
          zfleet::protocol::AuditEventType::agent_heartbeat,
          request.request_id, request.agent_id, "error",
          zfleet::protocol::SerializeAuditPayload(
              {{"transport", std::string("http2")},
               {"error_code",
                std::string(zfleet::protocol::ToString(
                    zfleet::protocol::ErrorCode::agent_not_registered))},
               {"message", std::string("agent not registered")}}));
      return NotFound("agent not registered");
    }

    database_->RecordHeartbeat(
        request, zfleet::protocol::SerializeHeartbeatRequest(request));
    RecordAuditEvent(
        zfleet::protocol::AuditEventType::agent_heartbeat, request.request_id,
        request.agent_id, "success",
        zfleet::protocol::SerializeAuditPayload(
            {{"transport", std::string("http2")},
             {"status", std::string("ok")},
             {"agent_version", request.agent_version}}));
    ZFLOG_INFO(EventLogger("http2.control.heartbeat", event),
               "heartbeat stored");
    return Accepted("ok");
  } catch (const std::exception& ex) {
    ZFLOG_ERROR(EventLogger("http2.control.heartbeat", event),
                "heartbeat failed: {}",
                ex.what());
    return InternalError();
  }
}

void Http2ControlService::RecordAuditEvent(
    zfleet::protocol::AuditEventType event_type,
    std::string request_id,
    std::string agent_id,
    std::string result,
    std::string payload_json) const {
  database_->RecordAuditEvent(zfleet::protocol::AuditEvent{
      .audit_id = zfleet::core::GenerateUuid(),
      .occurred_at = zfleet::core::NowUtcRfc3339(),
      .agent_id = std::move(agent_id),
      .request_id = std::move(request_id),
      .event_type = event_type,
      .result = std::move(result),
      .payload_json = std::move(payload_json),
  });
}

std::string_view ToString(ControlEventStatus status) noexcept {
  switch (status) {
    case ControlEventStatus::kAccepted:
      return "accepted";
    case ControlEventStatus::kInvalidArgument:
      return "invalid_argument";
    case ControlEventStatus::kNotFound:
      return "not_found";
    case ControlEventStatus::kInternalError:
      return "internal_error";
  }

  return "internal_error";
}

} // namespace zfleet::server
