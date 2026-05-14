#include "zfleet/protocol/message.h"
#include "zfleet/protocol/json_codec.h"

#include <nlohmann/json.hpp>

int main() {
  using nlohmann::json;
  using zfleet::protocol::AssetSnapshotRequest;
  using zfleet::protocol::AuditEvent;
  using zfleet::protocol::AuditEventType;
  using zfleet::protocol::ErrorCode;
  using zfleet::protocol::ErrorResponse;
  using zfleet::protocol::HeartbeatRequest;
  using zfleet::protocol::MessageKind;
  using zfleet::protocol::RegistrationRequest;
  using zfleet::protocol::StatusResponse;

  if (zfleet::protocol::protocol_version() != "v1") {
    return 1;
  }

  if (zfleet::protocol::ToString(MessageKind::heartbeat) != "heartbeat") {
    return 1;
  }

  if (zfleet::protocol::ToString(ErrorCode::agent_id_mismatch) !=
      "agent_id_mismatch") {
    return 1;
  }

  if (zfleet::protocol::ToString(AuditEventType::agent_asset_snapshot) !=
      "agent.asset_snapshot") {
    return 1;
  }

  if (zfleet::protocol::ErrorCodeFromString("internal_error") !=
      ErrorCode::internal_error) {
    return 1;
  }

  if (zfleet::protocol::AuditEventTypeFromString("agent.register") !=
      AuditEventType::agent_register) {
    return 1;
  }

  RegistrationRequest registration{
      .protocol_version = "v1",
      .request_id = "req-1",
      .agent_id = "agent-1",
      .occurred_at = "2026-05-13T10:15:30Z",
      .agent_version = "0.1.0",
      .hostname = "devbox-01",
      .os = "linux",
      .arch = "x86_64",
  };
  const auto registration_json = json(registration);
  const auto parsed_registration =
      registration_json.get<RegistrationRequest>();
  if (parsed_registration.agent_id != registration.agent_id ||
      parsed_registration.hostname != registration.hostname) {
    return 1;
  }

  HeartbeatRequest heartbeat{
      .protocol_version = "v1",
      .request_id = "req-2",
      .agent_id = "agent-1",
      .occurred_at = "2026-05-13T10:16:00Z",
      .agent_version = "0.1.0",
  };
  const auto heartbeat_json = json(heartbeat);
  const auto parsed_heartbeat = heartbeat_json.get<HeartbeatRequest>();
  if (parsed_heartbeat.request_id != heartbeat.request_id ||
      parsed_heartbeat.agent_version != heartbeat.agent_version) {
    return 1;
  }

  AssetSnapshotRequest asset{
      .protocol_version = "v1",
      .request_id = "req-3",
      .agent_id = "agent-1",
      .occurred_at = "2026-05-13T10:16:30Z",
      .hostname = "devbox-01",
      .os = "linux",
      .os_version = std::nullopt,
      .arch = "x86_64",
      .agent_version = "0.1.0",
  };
  const auto asset_json = json(asset);
  if (asset_json.contains("os_version")) {
    return 1;
  }
  const auto parsed_asset = asset_json.get<AssetSnapshotRequest>();
  if (parsed_asset.os_version.has_value() ||
      parsed_asset.arch != asset.arch) {
    return 1;
  }

  StatusResponse status{
      .protocol_version = "v1",
      .request_id = "req-4",
      .agent_id = "agent-1",
      .occurred_at = "2026-05-13T10:16:31Z",
      .status = "accepted",
      .server_time = "2026-05-13T10:16:31Z",
  };
  const auto parsed_status = json(status).get<StatusResponse>();
  if (parsed_status.status != "accepted") {
    return 1;
  }

  ErrorResponse error{
      .protocol_version = "v1",
      .request_id = "req-5",
      .agent_id = std::nullopt,
      .occurred_at = "2026-05-13T10:16:32Z",
      .error_code = ErrorCode::missing_required_field,
      .message = "missing field",
      .retryable = false,
  };
  const auto error_json = json(error);
  if (error_json.contains("agent_id")) {
    return 1;
  }
  const auto parsed_error = error_json.get<ErrorResponse>();
  if (parsed_error.error_code != ErrorCode::missing_required_field ||
      parsed_error.retryable) {
    return 1;
  }

  AuditEvent event{
      .audit_id = "audit-1",
      .occurred_at = "2026-05-13T10:16:33Z",
      .agent_id = "agent-1",
      .request_id = "req-6",
      .event_type = AuditEventType::agent_heartbeat,
      .result = "success",
      .payload_json = R"({"status":"ok"})",
  };
  const auto parsed_event = json(event).get<AuditEvent>();
  if (parsed_event.event_type != AuditEventType::agent_heartbeat ||
      !parsed_event.agent_id.has_value() ||
      parsed_event.payload_json != R"({"status":"ok"})") {
    return 1;
  }

  return 0;
}
