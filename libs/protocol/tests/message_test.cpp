#include "zfleet/protocol/json_codec.h"
#include "zfleet/protocol/message.h"

#include <catch2/catch_test_macros.hpp>

namespace {

using zfleet::protocol::AssetSnapshot;
using zfleet::protocol::AuditEvent;
using zfleet::protocol::AuditEventType;
using zfleet::protocol::CapabilityLevel;
using zfleet::protocol::CollectBasicInventoryInput;
using zfleet::protocol::CollectBasicInventoryResult;
using zfleet::protocol::ErrorCode;
using zfleet::protocol::AgentHeartbeat;
using zfleet::protocol::MessageKind;
using zfleet::protocol::AgentRegistration;
using zfleet::protocol::Task;
using zfleet::protocol::TaskCreation;
using zfleet::protocol::TaskError;
using zfleet::protocol::TaskExecutionStatus;
using zfleet::protocol::TaskRunning;
using zfleet::protocol::TaskResult;
using zfleet::protocol::TaskState;
using zfleet::protocol::TaskType;

} // namespace

TEST_CASE("protocol metadata and enum conversions are available") {
  REQUIRE(zfleet::protocol::protocol_version() == "v1");
  REQUIRE(zfleet::protocol::ToString(MessageKind::heartbeat) == "heartbeat");
  REQUIRE(zfleet::protocol::ToString(MessageKind::task_create) == "task_create");
  REQUIRE(zfleet::protocol::ToString(MessageKind::task_running) == "task_running");
  REQUIRE(zfleet::protocol::ToString(ErrorCode::agent_id_mismatch) ==
          "agent_id_mismatch");
  REQUIRE(zfleet::protocol::ToString(ErrorCode::task_execution_failed) ==
          "task_execution_failed");
  REQUIRE(zfleet::protocol::ToString(AuditEventType::agent_asset_snapshot) ==
          "agent.asset_snapshot");
  REQUIRE(zfleet::protocol::ToString(AuditEventType::task_assigned) ==
          "task.assigned");
  REQUIRE(zfleet::protocol::ToString(TaskType::collect_basic_inventory) ==
          "collect_basic_inventory");
  REQUIRE(zfleet::protocol::ToString(CapabilityLevel::readonly) == "readonly");
  REQUIRE(zfleet::protocol::ToString(TaskExecutionStatus::expired) ==
          "expired");
  REQUIRE(zfleet::protocol::ToString(TaskState::running) == "running");
  REQUIRE(zfleet::protocol::ErrorCodeFromString("internal_error") ==
          ErrorCode::internal_error);
  REQUIRE(zfleet::protocol::ErrorCodeFromString("task_not_found") ==
          ErrorCode::task_not_found);
  REQUIRE(zfleet::protocol::AuditEventTypeFromString("agent.register") ==
          AuditEventType::agent_register);
  REQUIRE(zfleet::protocol::AuditEventTypeFromString("task.expired") ==
          AuditEventType::task_expired);
  REQUIRE(zfleet::protocol::TaskTypeFromString("collect_basic_inventory") ==
          TaskType::collect_basic_inventory);
  REQUIRE(zfleet::protocol::CapabilityLevelFromString("shell") ==
          CapabilityLevel::shell);
  REQUIRE(zfleet::protocol::TaskExecutionStatusFromString("failed") ==
          TaskExecutionStatus::failed);
  REQUIRE(zfleet::protocol::TaskStateFromString("queued") ==
          TaskState::queued);
}

TEST_CASE("agent registration supports json round trip") {
  AgentRegistration registration{
      .protocol_version = "v1",
      .request_id = "req-1",
      .agent_id = "agent-1",
      .occurred_at = "2026-05-13T10:15:30Z",
      .agent_version = "0.1.0",
      .hostname = "devbox-01",
      .os = "linux",
      .arch = "x86_64",
  };

  const auto registration_json =
      zfleet::protocol::SerializeAgentRegistration(registration);
  const auto parsed_registration =
      zfleet::protocol::ParseAgentRegistration(registration_json);

  REQUIRE(std::holds_alternative<AgentRegistration>(parsed_registration));
  REQUIRE(std::get<AgentRegistration>(parsed_registration).agent_id ==
          registration.agent_id);
  REQUIRE(std::get<AgentRegistration>(parsed_registration).hostname ==
          registration.hostname);
}

TEST_CASE("agent heartbeat supports json round trip") {
  AgentHeartbeat heartbeat{
      .protocol_version = "v1",
      .request_id = "req-2",
      .agent_id = "agent-1",
      .occurred_at = "2026-05-13T10:16:00Z",
      .agent_version = "0.1.0",
  };

  const auto heartbeat_json =
      zfleet::protocol::SerializeAgentHeartbeat(heartbeat);
  const auto parsed_heartbeat =
      zfleet::protocol::ParseAgentHeartbeat(heartbeat_json);

  REQUIRE(std::holds_alternative<AgentHeartbeat>(parsed_heartbeat));
  REQUIRE(std::get<AgentHeartbeat>(parsed_heartbeat).request_id ==
          heartbeat.request_id);
  REQUIRE(std::get<AgentHeartbeat>(parsed_heartbeat).agent_version ==
          heartbeat.agent_version);
}

TEST_CASE("asset snapshot omits missing optional fields during json round trip") {
  AssetSnapshot asset{
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

  const auto asset_json =
      zfleet::protocol::SerializeAssetSnapshot(asset);
  const auto parsed_asset =
      zfleet::protocol::ParseAssetSnapshot(asset_json);

  REQUIRE(asset_json.find("os_version") == std::string::npos);
  REQUIRE(std::holds_alternative<AssetSnapshot>(parsed_asset));
  REQUIRE_FALSE(
      std::get<AssetSnapshot>(parsed_asset).os_version.has_value());
  REQUIRE(std::get<AssetSnapshot>(parsed_asset).arch == asset.arch);
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

  const auto parsed_event =
      zfleet::protocol::ParseAuditEvent(
          zfleet::protocol::SerializeAuditEvent(event));

  REQUIRE(std::holds_alternative<AuditEvent>(parsed_event));
  REQUIRE(std::get<AuditEvent>(parsed_event).event_type ==
          AuditEventType::agent_heartbeat);
  REQUIRE(std::get<AuditEvent>(parsed_event).agent_id.has_value());
  REQUIRE(std::get<AuditEvent>(parsed_event).payload_json ==
          R"({"status":"ok"})");
}

TEST_CASE("task supports json round trip") {
  Task task{
      .protocol_version = "v1",
      .task_id = "task-1",
      .agent_id = "agent-1",
      .task_type = TaskType::collect_basic_inventory,
      .capability_level = CapabilityLevel::readonly,
      .created_at = "2026-05-16T10:00:00Z",
      .expires_at = "2026-05-16T10:05:00Z",
      .input = CollectBasicInventoryInput{},
  };

  const auto task_json = zfleet::protocol::SerializeTask(task);
  const auto parsed_task = zfleet::protocol::ParseTask(task_json);

  REQUIRE(std::holds_alternative<Task>(parsed_task));
  REQUIRE(std::get<Task>(parsed_task).task_type ==
          TaskType::collect_basic_inventory);
  REQUIRE(std::get<Task>(parsed_task).capability_level ==
          CapabilityLevel::readonly);
  REQUIRE(std::holds_alternative<CollectBasicInventoryInput>(
      std::get<Task>(parsed_task).input));
}

TEST_CASE("task creation supports json round trip") {
  TaskCreation creation{
      .protocol_version = "v1",
      .request_id = "task-create-1",
      .occurred_at = "2026-05-17T10:00:00Z",
      .task =
          Task{
              .protocol_version = "v1",
              .task_id = "task-1",
              .agent_id = "agent-1",
              .task_type = TaskType::collect_basic_inventory,
              .capability_level = CapabilityLevel::readonly,
              .created_at = "2026-05-17T10:00:00Z",
              .expires_at = "2026-05-17T10:05:00Z",
              .input = CollectBasicInventoryInput{},
          },
  };

  const auto creation_json =
      zfleet::protocol::SerializeTaskCreation(creation);
  const auto parsed_creation =
      zfleet::protocol::ParseTaskCreation(creation_json);

  REQUIRE(std::holds_alternative<TaskCreation>(parsed_creation));
  REQUIRE(std::get<TaskCreation>(parsed_creation).request_id ==
          "task-create-1");
  REQUIRE(std::get<TaskCreation>(parsed_creation).task.task_id == "task-1");
}

TEST_CASE("task result supports success and failure shapes") {
  TaskResult success{
      .protocol_version = "v1",
      .request_id = "result-1",
      .task_id = "task-1",
      .agent_id = "agent-1",
      .task_type = TaskType::collect_basic_inventory,
      .occurred_at = "2026-05-16T10:00:30Z",
      .status = TaskExecutionStatus::succeeded,
      .result = CollectBasicInventoryResult{
          .hostname = "devbox-01",
          .os = "linux",
          .arch = "x86_64",
          .agent_version = "0.1.0",
      },
      .error = std::nullopt,
  };

  const auto success_json =
      zfleet::protocol::SerializeTaskResult(success);
  const auto parsed_success =
      zfleet::protocol::ParseTaskResult(success_json);

  REQUIRE(success_json.find("\"result\"") != std::string::npos);
  REQUIRE(success_json.find("\"error\"") == std::string::npos);
  REQUIRE(std::holds_alternative<TaskResult>(parsed_success));
  REQUIRE(std::get<TaskResult>(parsed_success).status ==
          TaskExecutionStatus::succeeded);
  REQUIRE(std::get<TaskResult>(parsed_success).result.has_value());
  REQUIRE_FALSE(std::get<TaskResult>(parsed_success).error.has_value());
  REQUIRE(std::get<TaskResult>(parsed_success).task_type ==
          TaskType::collect_basic_inventory);
  REQUIRE(std::holds_alternative<CollectBasicInventoryResult>(
      *std::get<TaskResult>(parsed_success).result));
  REQUIRE(std::get<CollectBasicInventoryResult>(
              *std::get<TaskResult>(parsed_success).result)
              .hostname == "devbox-01");

  TaskResult failure{
      .protocol_version = "v1",
      .request_id = "result-2",
      .task_id = "task-1",
      .agent_id = "agent-1",
      .task_type = TaskType::collect_basic_inventory,
      .occurred_at = "2026-05-16T10:00:31Z",
      .status = TaskExecutionStatus::failed,
      .result = std::nullopt,
      .error = TaskError{
          .error_code = ErrorCode::task_execution_failed,
          .message = "hostname missing",
          .retryable = false,
      },
  };

  const auto failure_json =
      zfleet::protocol::SerializeTaskResult(failure);
  const auto parsed_failure =
      zfleet::protocol::ParseTaskResult(failure_json);

  REQUIRE(failure_json.find("\"result\"") == std::string::npos);
  REQUIRE(failure_json.find("\"error\"") != std::string::npos);
  REQUIRE(std::holds_alternative<TaskResult>(parsed_failure));
  REQUIRE(std::get<TaskResult>(parsed_failure).status ==
          TaskExecutionStatus::failed);
  REQUIRE_FALSE(std::get<TaskResult>(parsed_failure).result.has_value());
  REQUIRE(std::get<TaskResult>(parsed_failure).error.has_value());
  REQUIRE(std::get<TaskResult>(parsed_failure).error->error_code ==
          ErrorCode::task_execution_failed);
}

TEST_CASE("task running supports json round trip") {
  TaskRunning running{
      .protocol_version = "v1",
      .request_id = "task-running-1",
      .task_id = "task-1",
      .agent_id = "agent-1",
      .task_type = TaskType::collect_basic_inventory,
      .occurred_at = "2026-05-17T10:00:10Z",
  };

  const auto running_json =
      zfleet::protocol::SerializeTaskRunning(running);
  const auto parsed_running =
      zfleet::protocol::ParseTaskRunning(running_json);

  REQUIRE(std::holds_alternative<TaskRunning>(parsed_running));
  REQUIRE(std::get<TaskRunning>(parsed_running).task_id == "task-1");
  REQUIRE(std::get<TaskRunning>(parsed_running).task_type ==
          TaskType::collect_basic_inventory);
}
