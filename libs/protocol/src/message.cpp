#include "zfleet/protocol/message.h"

namespace zfleet::protocol {

std::string_view protocol_version() noexcept {
  return "v1";
}

std::string_view ToString(MessageKind kind) noexcept {
  switch (kind) {
    case MessageKind::registration:
      return "registration";
    case MessageKind::heartbeat:
      return "heartbeat";
    case MessageKind::asset_snapshot:
      return "asset_snapshot";
  }

  return "unknown";
}

std::string_view ToString(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::invalid_json:
      return "invalid_json";
    case ErrorCode::missing_required_field:
      return "missing_required_field";
    case ErrorCode::invalid_field_type:
      return "invalid_field_type";
    case ErrorCode::unsupported_protocol_version:
      return "unsupported_protocol_version";
    case ErrorCode::agent_id_mismatch:
      return "agent_id_mismatch";
    case ErrorCode::agent_not_registered:
      return "agent_not_registered";
    case ErrorCode::internal_error:
      return "internal_error";
  }

  return "unknown";
}

std::string_view ToString(AuditEventType type) noexcept {
  switch (type) {
    case AuditEventType::agent_register:
      return "agent.register";
    case AuditEventType::agent_heartbeat:
      return "agent.heartbeat";
    case AuditEventType::agent_asset_snapshot:
      return "agent.asset_snapshot";
  }

  return "unknown";
}

std::optional<ErrorCode> ErrorCodeFromString(std::string_view code) noexcept {
  if (code == "invalid_json") {
    return ErrorCode::invalid_json;
  }
  if (code == "missing_required_field") {
    return ErrorCode::missing_required_field;
  }
  if (code == "invalid_field_type") {
    return ErrorCode::invalid_field_type;
  }
  if (code == "unsupported_protocol_version") {
    return ErrorCode::unsupported_protocol_version;
  }
  if (code == "agent_id_mismatch") {
    return ErrorCode::agent_id_mismatch;
  }
  if (code == "agent_not_registered") {
    return ErrorCode::agent_not_registered;
  }
  if (code == "internal_error") {
    return ErrorCode::internal_error;
  }

  return std::nullopt;
}

std::optional<AuditEventType> AuditEventTypeFromString(
    std::string_view type) noexcept {
  if (type == "agent.register") {
    return AuditEventType::agent_register;
  }
  if (type == "agent.heartbeat") {
    return AuditEventType::agent_heartbeat;
  }
  if (type == "agent.asset_snapshot") {
    return AuditEventType::agent_asset_snapshot;
  }

  return std::nullopt;
}

} // namespace zfleet::protocol
