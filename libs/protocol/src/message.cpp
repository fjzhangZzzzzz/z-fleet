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
    case MessageKind::task_create:
      return "task_create";
    case MessageKind::task_running:
      return "task_running";
    case MessageKind::task_result:
      return "task_result";
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
    case ErrorCode::task_not_found:
      return "task_not_found";
    case ErrorCode::task_agent_mismatch:
      return "task_agent_mismatch";
    case ErrorCode::task_already_finished:
      return "task_already_finished";
    case ErrorCode::task_expired:
      return "task_expired";
    case ErrorCode::unsupported_task_type:
      return "unsupported_task_type";
    case ErrorCode::capability_not_allowed:
      return "capability_not_allowed";
    case ErrorCode::task_execution_failed:
      return "task_execution_failed";
    case ErrorCode::task_result_invalid:
      return "task_result_invalid";
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
    case AuditEventType::task_queued:
      return "task.queued";
    case AuditEventType::task_assigned:
      return "task.assigned";
    case AuditEventType::task_running:
      return "task.running";
    case AuditEventType::task_succeeded:
      return "task.succeeded";
    case AuditEventType::task_failed:
      return "task.failed";
    case AuditEventType::task_expired:
      return "task.expired";
  }

  return "unknown";
}

std::string_view ToString(TaskType type) noexcept {
  switch (type) {
    case TaskType::collect_basic_inventory:
      return "collect_basic_inventory";
  }

  return "unknown";
}

std::string_view ToString(CapabilityLevel level) noexcept {
  switch (level) {
    case CapabilityLevel::readonly:
      return "readonly";
    case CapabilityLevel::low_risk_write:
      return "low_risk_write";
    case CapabilityLevel::high_risk_write:
      return "high_risk_write";
    case CapabilityLevel::shell:
      return "shell";
  }

  return "unknown";
}

std::string_view ToString(TaskExecutionStatus status) noexcept {
  switch (status) {
    case TaskExecutionStatus::succeeded:
      return "succeeded";
    case TaskExecutionStatus::failed:
      return "failed";
    case TaskExecutionStatus::expired:
      return "expired";
  }

  return "unknown";
}

std::string_view ToString(TaskState state) noexcept {
  switch (state) {
    case TaskState::queued:
      return "queued";
    case TaskState::assigned:
      return "assigned";
    case TaskState::running:
      return "running";
    case TaskState::succeeded:
      return "succeeded";
    case TaskState::failed:
      return "failed";
    case TaskState::expired:
      return "expired";
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
  if (code == "task_not_found") {
    return ErrorCode::task_not_found;
  }
  if (code == "task_agent_mismatch") {
    return ErrorCode::task_agent_mismatch;
  }
  if (code == "task_already_finished") {
    return ErrorCode::task_already_finished;
  }
  if (code == "task_expired") {
    return ErrorCode::task_expired;
  }
  if (code == "unsupported_task_type") {
    return ErrorCode::unsupported_task_type;
  }
  if (code == "capability_not_allowed") {
    return ErrorCode::capability_not_allowed;
  }
  if (code == "task_execution_failed") {
    return ErrorCode::task_execution_failed;
  }
  if (code == "task_result_invalid") {
    return ErrorCode::task_result_invalid;
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
  if (type == "task.queued") {
    return AuditEventType::task_queued;
  }
  if (type == "task.assigned") {
    return AuditEventType::task_assigned;
  }
  if (type == "task.running") {
    return AuditEventType::task_running;
  }
  if (type == "task.succeeded") {
    return AuditEventType::task_succeeded;
  }
  if (type == "task.failed") {
    return AuditEventType::task_failed;
  }
  if (type == "task.expired") {
    return AuditEventType::task_expired;
  }

  return std::nullopt;
}

std::optional<TaskType> TaskTypeFromString(std::string_view type) noexcept {
  if (type == "collect_basic_inventory") {
    return TaskType::collect_basic_inventory;
  }

  return std::nullopt;
}

std::optional<CapabilityLevel> CapabilityLevelFromString(
    std::string_view level) noexcept {
  if (level == "readonly") {
    return CapabilityLevel::readonly;
  }
  if (level == "low_risk_write") {
    return CapabilityLevel::low_risk_write;
  }
  if (level == "high_risk_write") {
    return CapabilityLevel::high_risk_write;
  }
  if (level == "shell") {
    return CapabilityLevel::shell;
  }

  return std::nullopt;
}

std::optional<TaskExecutionStatus> TaskExecutionStatusFromString(
    std::string_view status) noexcept {
  if (status == "succeeded") {
    return TaskExecutionStatus::succeeded;
  }
  if (status == "failed") {
    return TaskExecutionStatus::failed;
  }
  if (status == "expired") {
    return TaskExecutionStatus::expired;
  }

  return std::nullopt;
}

std::optional<TaskState> TaskStateFromString(std::string_view state) noexcept {
  if (state == "queued") {
    return TaskState::queued;
  }
  if (state == "assigned") {
    return TaskState::assigned;
  }
  if (state == "running") {
    return TaskState::running;
  }
  if (state == "succeeded") {
    return TaskState::succeeded;
  }
  if (state == "failed") {
    return TaskState::failed;
  }
  if (state == "expired") {
    return TaskState::expired;
  }

  return std::nullopt;
}

} // namespace zfleet::protocol
