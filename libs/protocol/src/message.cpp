#include "zfleet/protocol/message.h"

namespace zfleet::protocol {

std::string_view protocol_version() noexcept {
  return "v1";
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
    case ErrorCode::package_not_found:
      return "package_not_found";
    case ErrorCode::package_retired:
      return "package_retired";
    case ErrorCode::platform_arch_mismatch:
      return "platform_arch_mismatch";
    case ErrorCode::build_type_not_allowed:
      return "build_type_not_allowed";
    case ErrorCode::installer_too_old:
      return "installer_too_old";
    case ErrorCode::download_failed:
      return "download_failed";
    case ErrorCode::checksum_mismatch:
      return "checksum_mismatch";
    case ErrorCode::apply_failed:
      return "apply_failed";
    case ErrorCode::start_new_agent_failed:
      return "start_new_agent_failed";
    case ErrorCode::waiting_reconnect_timeout:
      return "waiting_reconnect_timeout";
    case ErrorCode::agent_reported_unexpected_version:
      return "agent_reported_unexpected_version";
  }

  return "unknown";
}

std::string_view ToString(AuditEventType type) noexcept {
  switch (type) {
    case AuditEventType::agent_register:
      return "agent.register";
    case AuditEventType::agent_asset_snapshot:
      return "agent.asset_snapshot";
    case AuditEventType::package_validated:
      return "package.validated";
    case AuditEventType::package_published:
      return "package.published";
    case AuditEventType::package_retired:
      return "package.retired";
    case AuditEventType::agent_upgrade_requested:
      return "agent.upgrade_requested";
    case AuditEventType::agent_rollback_requested:
      return "agent.rollback_requested";
    case AuditEventType::agent_upgrade_confirmed:
      return "agent.upgrade_confirmed";
    case AuditEventType::registration_token_created:
      return "registration_token.created";
    case AuditEventType::registration_token_used:
      return "registration_token.used";
    case AuditEventType::registration_token_rejected:
      return "registration_token.rejected";
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
    case TaskType::package_update:
      return "package_update";
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
  if (code == "package_not_found") {
    return ErrorCode::package_not_found;
  }
  if (code == "package_retired") {
    return ErrorCode::package_retired;
  }
  if (code == "platform_arch_mismatch") {
    return ErrorCode::platform_arch_mismatch;
  }
  if (code == "build_type_not_allowed") {
    return ErrorCode::build_type_not_allowed;
  }
  if (code == "installer_too_old") {
    return ErrorCode::installer_too_old;
  }
  if (code == "download_failed") {
    return ErrorCode::download_failed;
  }
  if (code == "checksum_mismatch") {
    return ErrorCode::checksum_mismatch;
  }
  if (code == "apply_failed") {
    return ErrorCode::apply_failed;
  }
  if (code == "start_new_agent_failed") {
    return ErrorCode::start_new_agent_failed;
  }
  if (code == "waiting_reconnect_timeout") {
    return ErrorCode::waiting_reconnect_timeout;
  }
  if (code == "agent_reported_unexpected_version") {
    return ErrorCode::agent_reported_unexpected_version;
  }

  return std::nullopt;
}

std::optional<AuditEventType> AuditEventTypeFromString(
    std::string_view type) noexcept {
  if (type == "agent.register") {
    return AuditEventType::agent_register;
  }
  if (type == "agent.asset_snapshot") {
    return AuditEventType::agent_asset_snapshot;
  }
  if (type == "package.validated") {
    return AuditEventType::package_validated;
  }
  if (type == "package.published") {
    return AuditEventType::package_published;
  }
  if (type == "package.retired") {
    return AuditEventType::package_retired;
  }
  if (type == "agent.upgrade_requested") {
    return AuditEventType::agent_upgrade_requested;
  }
  if (type == "agent.rollback_requested") {
    return AuditEventType::agent_rollback_requested;
  }
  if (type == "agent.upgrade_confirmed") {
    return AuditEventType::agent_upgrade_confirmed;
  }
  if (type == "registration_token.created") {
    return AuditEventType::registration_token_created;
  }
  if (type == "registration_token.used") {
    return AuditEventType::registration_token_used;
  }
  if (type == "registration_token.rejected") {
    return AuditEventType::registration_token_rejected;
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
  if (type == "package_update") {
    return TaskType::package_update;
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
