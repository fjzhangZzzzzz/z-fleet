#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace zfleet::protocol {

enum class MessageKind {
  registration,
  heartbeat,
  asset_snapshot,
};

enum class ErrorCode {
  invalid_json,
  missing_required_field,
  invalid_field_type,
  unsupported_protocol_version,
  agent_id_mismatch,
  agent_not_registered,
  internal_error,
};

enum class AuditEventType {
  agent_register,
  agent_heartbeat,
  agent_asset_snapshot,
};

struct RegistrationRequest {
  std::string protocol_version;
  std::string request_id;
  std::string agent_id;
  std::string occurred_at;
  std::string agent_version;
  std::string hostname;
  std::string os;
  std::string arch;
};

struct HeartbeatRequest {
  std::string protocol_version;
  std::string request_id;
  std::string agent_id;
  std::string occurred_at;
  std::string agent_version;
};

struct AssetSnapshotRequest {
  std::string protocol_version;
  std::string request_id;
  std::string agent_id;
  std::string occurred_at;
  std::string hostname;
  std::string os;
  std::optional<std::string> os_version;
  std::string arch;
  std::string agent_version;
};

struct StatusResponse {
  std::string protocol_version;
  std::string request_id;
  std::string agent_id;
  std::string occurred_at;
  std::string status;
  std::string server_time;
};

struct ErrorResponse {
  std::string protocol_version;
  std::string request_id;
  std::optional<std::string> agent_id;
  std::string occurred_at;
  ErrorCode error_code;
  std::string message;
  bool retryable;
};

struct AuditEvent {
  std::string audit_id;
  std::string occurred_at;
  std::optional<std::string> agent_id;
  std::string request_id;
  AuditEventType event_type;
  std::string result;
  std::string payload_json;
};

std::string_view protocol_version() noexcept;
std::string_view ToString(MessageKind kind) noexcept;
std::string_view ToString(ErrorCode code) noexcept;
std::string_view ToString(AuditEventType type) noexcept;

std::optional<ErrorCode> ErrorCodeFromString(std::string_view code) noexcept;
std::optional<AuditEventType> AuditEventTypeFromString(
    std::string_view type) noexcept;

} // namespace zfleet::protocol
