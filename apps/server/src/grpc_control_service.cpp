#include "grpc_control_service.h"

#include "zfleet/core/log.h"
#include "zfleet/core/time.h"
#include "zfleet/core/uuid.h"
#include "zfleet/protocol/json_codec.h"
#include "zfleet/protocol/message.h"

#include <grpcpp/grpcpp.h>

#include <cstdint>
#include <stdexcept>
#include <utility>

namespace zfleet::server {
namespace {

namespace proto = zfleet::protocol::v1;

void RecordAuditEvent(ServerDatabase* database,
                      zfleet::protocol::AuditEventType event_type,
                      std::string request_id,
                      std::optional<std::string> agent_id,
                      std::string result,
                      std::string payload_json) {
  database->RecordAuditEvent(zfleet::protocol::AuditEvent{
      .audit_id = zfleet::core::GenerateUuid(),
      .occurred_at = zfleet::core::NowUtcRfc3339(),
      .agent_id = std::move(agent_id),
      .request_id = std::move(request_id),
      .event_type = event_type,
      .result = std::move(result),
      .payload_json = std::move(payload_json),
  });
}

grpc::Status InvalidArgument(std::string message) {
  return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, std::move(message));
}

grpc::Status FailedPrecondition(std::string message) {
  return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                      std::move(message));
}

grpc::Status NotFound(std::string message) {
  return grpc::Status(grpc::StatusCode::NOT_FOUND, std::move(message));
}

grpc::Status InternalError() {
  return grpc::Status(grpc::StatusCode::INTERNAL, "internal error");
}

grpc::Status ValidateEnvelope(const proto::AgentEvent& event,
                              proto::AgentEvent::PayloadCase expected_payload) {
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
  if (event.payload_case() != expected_payload) {
    return InvalidArgument("unexpected agent event payload");
  }
  return grpc::Status::OK;
}

} // namespace

GrpcControlService::GrpcControlService(ServerDatabase* database)
    : database_(database) {
  if (database_ == nullptr) {
    throw std::invalid_argument("server database must not be null");
  }
}

grpc::Status GrpcControlService::Connect(
    grpc::ServerContext* /*context*/,
    grpc::ServerReaderWriter<proto::ServerCommand, proto::AgentEvent>* stream) {
  std::string active_agent_id;
  proto::AgentEvent event;
  while (stream->Read(&event)) {
    grpc::Status status;
    switch (event.payload_case()) {
      case proto::AgentEvent::kRegister:
        status = HandleRegister(event, &active_agent_id);
        break;
      case proto::AgentEvent::kHeartbeat:
        status = HandleHeartbeat(event);
        break;
      default:
        status = InvalidArgument("unsupported agent event payload");
        break;
    }
    if (!status.ok()) {
      if (!active_agent_id.empty()) {
        MarkDisconnected(active_agent_id);
      }
      return status;
    }
  }

  if (!active_agent_id.empty()) {
    MarkDisconnected(active_agent_id);
  }
  return grpc::Status::OK;
}

std::optional<ControlConnectionInfo> GrpcControlService::FindConnection(
    const std::string& agent_id) const {
  std::lock_guard lock(connections_mutex_);
  const auto it = connections_.find(agent_id);
  if (it == connections_.end()) {
    return std::nullopt;
  }
  return it->second.info;
}

bool GrpcControlService::IsAgentOnline(
    const std::string& agent_id,
    std::chrono::seconds heartbeat_timeout) const {
  std::lock_guard lock(connections_mutex_);
  const auto it = connections_.find(agent_id);
  if (it == connections_.end() || it->second.info.last_heartbeat_at.empty()) {
    return false;
  }
  return std::chrono::steady_clock::now() -
             it->second.last_heartbeat_monotonic <=
         heartbeat_timeout;
}

grpc::Status GrpcControlService::HandleRegister(
    const proto::AgentEvent& event,
    std::string* active_agent_id) {
  const auto validation =
      ValidateEnvelope(event, proto::AgentEvent::kRegister);
  if (!validation.ok()) {
    return validation;
  }

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
        database_, zfleet::protocol::AuditEventType::agent_register,
        request.request_id, request.agent_id, "success",
        zfleet::protocol::SerializeAuditPayload(
            {{"grpc_status", std::int64_t{0}},
             {"status", std::string("accepted")},
             {"agent_version", request.agent_version},
             {"hostname", request.hostname},
             {"os", request.os},
             {"arch", request.arch}}));

    const auto now = zfleet::core::NowUtcRfc3339();
    {
      std::lock_guard lock(connections_mutex_);
      auto& stored = connections_[request.agent_id];
      stored.info.agent_id = request.agent_id;
      stored.info.connected_at = now;
      stored.info.connected = true;
    }
    *active_agent_id = request.agent_id;

    ZFLOG_INFO(zfleet::core::log::Component("server").With(
                   {{"route", "grpc.connect.register"},
                    {"request_id", request.request_id},
                    {"agent_id", request.agent_id}}),
               "grpc registration accepted");
    return grpc::Status::OK;
  } catch (const std::exception& ex) {
    ZFLOG_ERROR(zfleet::core::log::Component("server").With(
                    {"route", "grpc.connect.register"}),
                "grpc register failed: {}",
                ex.what());
    return InternalError();
  }
}

grpc::Status GrpcControlService::HandleHeartbeat(
    const proto::AgentEvent& event) {
  const auto validation =
      ValidateEnvelope(event, proto::AgentEvent::kHeartbeat);
  if (!validation.ok()) {
    return validation;
  }

  {
    std::lock_guard lock(connections_mutex_);
    const auto it = connections_.find(event.agent_id());
    if (it == connections_.end() || !it->second.info.connected) {
      return FailedPrecondition("agent must register before heartbeat");
    }
  }

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
      return NotFound("agent not registered");
    }

    database_->RecordHeartbeat(
        request, zfleet::protocol::SerializeHeartbeatRequest(request));
    RecordAuditEvent(
        database_, zfleet::protocol::AuditEventType::agent_heartbeat,
        request.request_id, request.agent_id, "success",
        zfleet::protocol::SerializeAuditPayload(
            {{"grpc_status", std::int64_t{0}},
             {"status", std::string("ok")},
             {"agent_version", request.agent_version}}));

    {
      std::lock_guard lock(connections_mutex_);
      auto& stored = connections_[request.agent_id];
      stored.info.agent_id = request.agent_id;
      stored.info.last_heartbeat_at = request.occurred_at;
      stored.info.connected = true;
      stored.last_heartbeat_monotonic = std::chrono::steady_clock::now();
    }

    ZFLOG_INFO(zfleet::core::log::Component("server").With(
                   {{"route", "grpc.connect.heartbeat"},
                    {"request_id", request.request_id},
                    {"agent_id", request.agent_id}}),
               "grpc heartbeat stored");
    return grpc::Status::OK;
  } catch (const std::exception& ex) {
    ZFLOG_ERROR(zfleet::core::log::Component("server").With(
                    {"route", "grpc.connect.heartbeat"}),
                "grpc heartbeat failed: {}",
                ex.what());
    return InternalError();
  }
}

void GrpcControlService::MarkDisconnected(const std::string& agent_id) {
  std::lock_guard lock(connections_mutex_);
  const auto it = connections_.find(agent_id);
  if (it != connections_.end()) {
    it->second.info.connected = false;
  }
}

} // namespace zfleet::server
