#include "zfleet/protocol/json_codec.h"
#include "zfleet/protocol/message.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

namespace {

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

} // namespace

TEST_CASE("protocol metadata and enum conversions are available") {
  REQUIRE(zfleet::protocol::protocol_version() == "v1");
  REQUIRE(zfleet::protocol::ToString(MessageKind::heartbeat) == "heartbeat");
  REQUIRE(zfleet::protocol::ToString(ErrorCode::agent_id_mismatch) ==
          "agent_id_mismatch");
  REQUIRE(zfleet::protocol::ToString(AuditEventType::agent_asset_snapshot) ==
          "agent.asset_snapshot");
  REQUIRE(zfleet::protocol::ErrorCodeFromString("internal_error") ==
          ErrorCode::internal_error);
  REQUIRE(zfleet::protocol::AuditEventTypeFromString("agent.register") ==
          AuditEventType::agent_register);
}

TEST_CASE("registration request supports json round trip") {
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
  const auto parsed_registration = registration_json.get<RegistrationRequest>();

  REQUIRE(parsed_registration.agent_id == registration.agent_id);
  REQUIRE(parsed_registration.hostname == registration.hostname);
}

TEST_CASE("heartbeat request supports json round trip") {
  HeartbeatRequest heartbeat{
      .protocol_version = "v1",
      .request_id = "req-2",
      .agent_id = "agent-1",
      .occurred_at = "2026-05-13T10:16:00Z",
      .agent_version = "0.1.0",
  };

  const auto heartbeat_json = json(heartbeat);
  const auto parsed_heartbeat = heartbeat_json.get<HeartbeatRequest>();

  REQUIRE(parsed_heartbeat.request_id == heartbeat.request_id);
  REQUIRE(parsed_heartbeat.agent_version == heartbeat.agent_version);
}

TEST_CASE("asset snapshot omits missing optional fields during json round trip") {
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
  const auto parsed_asset = asset_json.get<AssetSnapshotRequest>();

  REQUIRE_FALSE(asset_json.contains("os_version"));
  REQUIRE_FALSE(parsed_asset.os_version.has_value());
  REQUIRE(parsed_asset.arch == asset.arch);
}

TEST_CASE("status response supports json round trip") {
  StatusResponse status{
      .protocol_version = "v1",
      .request_id = "req-4",
      .agent_id = "agent-1",
      .occurred_at = "2026-05-13T10:16:31Z",
      .status = "accepted",
      .server_time = "2026-05-13T10:16:31Z",
  };

  const auto parsed_status = json(status).get<StatusResponse>();
  REQUIRE(parsed_status.status == "accepted");
}

TEST_CASE("error response omits missing optional fields during json round trip") {
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
  const auto parsed_error = error_json.get<ErrorResponse>();

  REQUIRE_FALSE(error_json.contains("agent_id"));
  REQUIRE(parsed_error.error_code == ErrorCode::missing_required_field);
  REQUIRE_FALSE(parsed_error.retryable);
}

TEST_CASE("audit event supports json round trip") {
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

  REQUIRE(parsed_event.event_type == AuditEventType::agent_heartbeat);
  REQUIRE(parsed_event.agent_id.has_value());
  REQUIRE(parsed_event.payload_json == R"({"status":"ok"})");
}
