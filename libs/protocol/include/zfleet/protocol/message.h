#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace zfleet::protocol {

enum class ErrorCode {
  invalid_json,
  missing_required_field,
  invalid_field_type,
  unsupported_protocol_version,
  agent_id_mismatch,
  agent_not_registered,
  internal_error,
  task_not_found,
  task_agent_mismatch,
  task_already_finished,
  task_expired,
  unsupported_task_type,
  capability_not_allowed,
  task_execution_failed,
  task_result_invalid,
  package_not_found,
  package_retired,
  platform_arch_mismatch,
  build_type_not_allowed,
  installer_too_old,
  download_failed,
  checksum_mismatch,
  apply_failed,
  start_new_agent_failed,
  waiting_reconnect_timeout,
  agent_reported_unexpected_version,
};

enum class AuditEventType {
  agent_register,
  agent_asset_snapshot,
  package_validated,
  package_published,
  package_retired,
  agent_upgrade_requested,
  agent_rollback_requested,
  agent_upgrade_confirmed,
  registration_token_created,
  registration_token_used,
  registration_token_rejected,
  task_queued,
  task_assigned,
  task_running,
  task_succeeded,
  task_failed,
  task_expired,
};

enum class TaskType {
  collect_basic_inventory,
  package_update,
};

enum class CapabilityLevel {
  readonly,
  low_risk_write,
  high_risk_write,
  shell,
};

enum class TaskExecutionStatus {
  succeeded,
  failed,
  expired,
};

enum class TaskState {
  queued,
  assigned,
  running,
  succeeded,
  failed,
  expired,
};

struct AgentRegistration {
  std::string protocol_version;
  std::string request_id;
  std::string agent_id;
  std::string occurred_at;
  std::string agent_version;
  std::string hostname;
  std::string os;
  std::string arch;
  std::optional<std::string> registration_token;
};

struct AgentHeartbeat {
  std::string protocol_version;
  std::string request_id;
  std::string agent_id;
  std::string occurred_at;
  std::string agent_version;
};

struct AssetSnapshot {
  std::string protocol_version;
  std::string request_id;
  std::string agent_id;
  std::string occurred_at;
  std::string hostname;
  std::string os;
  std::optional<std::string> os_version;
  std::string arch;
  std::string agent_version;
  std::vector<std::string> applications;
  std::vector<std::string> services;
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

struct CollectBasicInventoryInput {};

struct CollectBasicInventoryResult {
  std::string hostname;
  std::string os;
  std::string arch;
  std::string agent_version;
};

struct PackageUpdateInput {
  std::string action = "apply";
  std::string component;
  std::string package_id;
  std::string version;
  std::string platform;
  std::string arch;
  std::string build_type;
  std::string package_url;
  std::string package_sha256;
  std::string manifest_sha256;
  std::string min_installer_version;
  bool allow_downgrade = false;
  bool force = false;
};

struct PackageUpdateResult {
  std::string component;
  std::string package_id;
  std::string version;
  std::string state;
  std::string error_code;
  std::string error_message;
};

using TaskInput = std::variant<CollectBasicInventoryInput, PackageUpdateInput>;
using TaskResultData =
    std::variant<CollectBasicInventoryResult, PackageUpdateResult>;

struct Task {
  std::string protocol_version;
  std::string task_id;
  std::string agent_id;
  TaskType task_type;
  CapabilityLevel capability_level;
  std::string created_at;
  std::string expires_at;
  TaskInput input;
};

struct TaskError {
  ErrorCode error_code;
  std::string message;
  bool retryable;
};

struct TaskRunning {
  std::string protocol_version;
  std::string request_id;
  std::string task_id;
  std::string agent_id;
  TaskType task_type;
  std::string occurred_at;
};

struct TaskResult {
  std::string protocol_version;
  std::string request_id;
  std::string task_id;
  std::string agent_id;
  TaskType task_type;
  std::string occurred_at;
  TaskExecutionStatus status;
  std::optional<TaskResultData> result;
  std::optional<TaskError> error;
};

using AgentEventPayload =
    std::variant<AgentRegistration, AgentHeartbeat, AssetSnapshot, TaskRunning,
                 TaskResult>;

struct AgentEvent {
  AgentEventPayload payload;
};

struct ServerError {
  ErrorCode error_code;
  std::string message;
  bool retryable;
};

using ServerCommandPayload = std::variant<Task, ServerError>;

struct ServerCommand {
  std::string protocol_version;
  std::string message_id;
  std::string correlation_id;
  std::string agent_id;
  std::string occurred_at;
  ServerCommandPayload payload;
};

std::string_view protocol_version() noexcept;
std::string_view ToString(ErrorCode code) noexcept;
std::string_view ToString(AuditEventType type) noexcept;
std::string_view ToString(TaskType type) noexcept;
std::string_view ToString(CapabilityLevel level) noexcept;
std::string_view ToString(TaskExecutionStatus status) noexcept;
std::string_view ToString(TaskState state) noexcept;

std::optional<ErrorCode> ErrorCodeFromString(std::string_view code) noexcept;
std::optional<AuditEventType> AuditEventTypeFromString(
    std::string_view type) noexcept;
std::optional<TaskType> TaskTypeFromString(std::string_view type) noexcept;
std::optional<CapabilityLevel> CapabilityLevelFromString(
    std::string_view level) noexcept;
std::optional<TaskExecutionStatus> TaskExecutionStatusFromString(
    std::string_view status) noexcept;
std::optional<TaskState> TaskStateFromString(std::string_view state) noexcept;

bool IsTerminalTaskState(TaskState state) noexcept;
bool CanTransitionTaskState(TaskState from, TaskState to) noexcept;
CapabilityLevel RequiredCapabilityForTaskType(TaskType task_type) noexcept;
bool TaskInputMatchesType(TaskType task_type, const TaskInput& input) noexcept;

std::string_view AgentEventProtocolVersion(const AgentEvent& event) noexcept;
std::string_view AgentEventRequestId(const AgentEvent& event) noexcept;
std::string_view AgentEventAgentId(const AgentEvent& event) noexcept;
std::string_view AgentEventOccurredAt(const AgentEvent& event) noexcept;

} // namespace zfleet::protocol
