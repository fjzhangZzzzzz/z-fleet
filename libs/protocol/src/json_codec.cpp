#include "zfleet/protocol/json_codec.h"

#include <stdexcept>
#include <string>

namespace zfleet::protocol {
namespace {

using nlohmann::json;

template <typename T>
T required(const json& j, const char* key) {
  return j.at(key).get<T>();
}

template <typename T>
void assign_optional(const json& j, const char* key, std::optional<T>& out) {
  if (!j.contains(key) || j.at(key).is_null()) {
    out.reset();
    return;
  }

  out = j.at(key).get<T>();
}

ErrorCode parse_error_code(const std::string& code) {
  const auto parsed = ErrorCodeFromString(code);
  if (!parsed.has_value()) {
    throw std::invalid_argument("unknown error_code: " + code);
  }

  return *parsed;
}

AuditEventType parse_audit_event_type(const std::string& type) {
  const auto parsed = AuditEventTypeFromString(type);
  if (!parsed.has_value()) {
    throw std::invalid_argument("unknown event_type: " + type);
  }

  return *parsed;
}

} // namespace

void to_json(nlohmann::json& j, const RegistrationRequest& request) {
  j = {
      {"protocol_version", request.protocol_version},
      {"request_id", request.request_id},
      {"agent_id", request.agent_id},
      {"occurred_at", request.occurred_at},
      {"agent_version", request.agent_version},
      {"hostname", request.hostname},
      {"os", request.os},
      {"arch", request.arch},
  };
}

void from_json(const nlohmann::json& j, RegistrationRequest& request) {
  request.protocol_version = required<std::string>(j, "protocol_version");
  request.request_id = required<std::string>(j, "request_id");
  request.agent_id = required<std::string>(j, "agent_id");
  request.occurred_at = required<std::string>(j, "occurred_at");
  request.agent_version = required<std::string>(j, "agent_version");
  request.hostname = required<std::string>(j, "hostname");
  request.os = required<std::string>(j, "os");
  request.arch = required<std::string>(j, "arch");
}

void to_json(nlohmann::json& j, const HeartbeatRequest& request) {
  j = {
      {"protocol_version", request.protocol_version},
      {"request_id", request.request_id},
      {"agent_id", request.agent_id},
      {"occurred_at", request.occurred_at},
      {"agent_version", request.agent_version},
  };
}

void from_json(const nlohmann::json& j, HeartbeatRequest& request) {
  request.protocol_version = required<std::string>(j, "protocol_version");
  request.request_id = required<std::string>(j, "request_id");
  request.agent_id = required<std::string>(j, "agent_id");
  request.occurred_at = required<std::string>(j, "occurred_at");
  request.agent_version = required<std::string>(j, "agent_version");
}

void to_json(nlohmann::json& j, const AssetSnapshotRequest& request) {
  j = {
      {"protocol_version", request.protocol_version},
      {"request_id", request.request_id},
      {"agent_id", request.agent_id},
      {"occurred_at", request.occurred_at},
      {"hostname", request.hostname},
      {"os", request.os},
      {"arch", request.arch},
      {"agent_version", request.agent_version},
  };

  if (request.os_version.has_value()) {
    j["os_version"] = *request.os_version;
  }
}

void from_json(const nlohmann::json& j, AssetSnapshotRequest& request) {
  request.protocol_version = required<std::string>(j, "protocol_version");
  request.request_id = required<std::string>(j, "request_id");
  request.agent_id = required<std::string>(j, "agent_id");
  request.occurred_at = required<std::string>(j, "occurred_at");
  request.hostname = required<std::string>(j, "hostname");
  request.os = required<std::string>(j, "os");
  assign_optional(j, "os_version", request.os_version);
  request.arch = required<std::string>(j, "arch");
  request.agent_version = required<std::string>(j, "agent_version");
}

void to_json(nlohmann::json& j, const StatusResponse& response) {
  j = {
      {"protocol_version", response.protocol_version},
      {"request_id", response.request_id},
      {"agent_id", response.agent_id},
      {"occurred_at", response.occurred_at},
      {"status", response.status},
      {"server_time", response.server_time},
  };
}

void from_json(const nlohmann::json& j, StatusResponse& response) {
  response.protocol_version = required<std::string>(j, "protocol_version");
  response.request_id = required<std::string>(j, "request_id");
  response.agent_id = required<std::string>(j, "agent_id");
  response.occurred_at = required<std::string>(j, "occurred_at");
  response.status = required<std::string>(j, "status");
  response.server_time = required<std::string>(j, "server_time");
}

void to_json(nlohmann::json& j, const ErrorResponse& response) {
  j = {
      {"protocol_version", response.protocol_version},
      {"request_id", response.request_id},
      {"occurred_at", response.occurred_at},
      {"error_code", ToString(response.error_code)},
      {"message", response.message},
      {"retryable", response.retryable},
  };

  if (response.agent_id.has_value()) {
    j["agent_id"] = *response.agent_id;
  }
}

void from_json(const nlohmann::json& j, ErrorResponse& response) {
  response.protocol_version = required<std::string>(j, "protocol_version");
  response.request_id = required<std::string>(j, "request_id");
  assign_optional(j, "agent_id", response.agent_id);
  response.occurred_at = required<std::string>(j, "occurred_at");
  response.error_code =
      parse_error_code(required<std::string>(j, "error_code"));
  response.message = required<std::string>(j, "message");
  response.retryable = required<bool>(j, "retryable");
}

void to_json(nlohmann::json& j, const AuditEvent& event) {
  j = {
      {"audit_id", event.audit_id},
      {"occurred_at", event.occurred_at},
      {"request_id", event.request_id},
      {"event_type", ToString(event.event_type)},
      {"result", event.result},
      {"payload_json", event.payload_json},
  };

  if (event.agent_id.has_value()) {
    j["agent_id"] = *event.agent_id;
  }
}

void from_json(const nlohmann::json& j, AuditEvent& event) {
  event.audit_id = required<std::string>(j, "audit_id");
  event.occurred_at = required<std::string>(j, "occurred_at");
  assign_optional(j, "agent_id", event.agent_id);
  event.request_id = required<std::string>(j, "request_id");
  event.event_type =
      parse_audit_event_type(required<std::string>(j, "event_type"));
  event.result = required<std::string>(j, "result");
  event.payload_json = required<std::string>(j, "payload_json");
}

} // namespace zfleet::protocol
