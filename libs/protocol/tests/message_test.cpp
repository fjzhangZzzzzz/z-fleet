#include "zfleet/protocol/json_codec.h"
#include "zfleet/protocol/message.h"

#include <catch2/catch_test_macros.hpp>

namespace {

using zfleet::protocol::AssetSnapshotRequest;
using zfleet::protocol::AuditEvent;
using zfleet::protocol::AuditEventType;
using zfleet::protocol::CapabilityLevel;
using zfleet::protocol::CollectBasicInventoryInput;
using zfleet::protocol::CollectBasicInventoryResult;
using zfleet::protocol::ErrorCode;
using zfleet::protocol::ErrorResponse;
using zfleet::protocol::HeartbeatRequest;
using zfleet::protocol::MessageKind;
using zfleet::protocol::RegistrationRequest;
using zfleet::protocol::StatusResponse;
using zfleet::protocol::Task;
using zfleet::protocol::TaskCreateRequest;
using zfleet::protocol::TaskError;
using zfleet::protocol::TaskExecutionStatus;
using zfleet::protocol::TaskPollResponse;
using zfleet::protocol::TaskPollStatus;
using zfleet::protocol::TaskRunningRequest;
using zfleet::protocol::TaskResultRequest;
using zfleet::protocol::TaskState;
using zfleet::protocol::TaskType;

} // namespace

TEST_CASE("protocol metadata and enum conversions are available") {
  REQUIRE(zfleet::protocol::protocol_version() == "v1");
  REQUIRE(zfleet::protocol::ToString(MessageKind::heartbeat) == "heartbeat");
  REQUIRE(zfleet::protocol::ToString(MessageKind::task_create) == "task_create");
  REQUIRE(zfleet::protocol::ToString(MessageKind::task_poll) == "task_poll");
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
  REQUIRE(zfleet::protocol::ToString(TaskPollStatus::assigned) == "assigned");
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
  REQUIRE(zfleet::protocol::TaskPollStatusFromString("idle") ==
          TaskPollStatus::idle);
  REQUIRE(zfleet::protocol::TaskExecutionStatusFromString("failed") ==
          TaskExecutionStatus::failed);
  REQUIRE(zfleet::protocol::TaskStateFromString("queued") ==
          TaskState::queued);
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

  const auto registration_json =
      zfleet::protocol::SerializeRegistrationRequest(registration);
  const auto parsed_registration =
      zfleet::protocol::ParseRegistrationRequest(registration_json);

  REQUIRE(std::holds_alternative<RegistrationRequest>(parsed_registration));
  REQUIRE(std::get<RegistrationRequest>(parsed_registration).agent_id ==
          registration.agent_id);
  REQUIRE(std::get<RegistrationRequest>(parsed_registration).hostname ==
          registration.hostname);
}

TEST_CASE("heartbeat request supports json round trip") {
  HeartbeatRequest heartbeat{
      .protocol_version = "v1",
      .request_id = "req-2",
      .agent_id = "agent-1",
      .occurred_at = "2026-05-13T10:16:00Z",
      .agent_version = "0.1.0",
  };

  const auto heartbeat_json =
      zfleet::protocol::SerializeHeartbeatRequest(heartbeat);
  const auto parsed_heartbeat =
      zfleet::protocol::ParseHeartbeatRequest(heartbeat_json);

  REQUIRE(std::holds_alternative<HeartbeatRequest>(parsed_heartbeat));
  REQUIRE(std::get<HeartbeatRequest>(parsed_heartbeat).request_id ==
          heartbeat.request_id);
  REQUIRE(std::get<HeartbeatRequest>(parsed_heartbeat).agent_version ==
          heartbeat.agent_version);
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

  const auto asset_json =
      zfleet::protocol::SerializeAssetSnapshotRequest(asset);
  const auto parsed_asset =
      zfleet::protocol::ParseAssetSnapshotRequest(asset_json);

  REQUIRE(asset_json.find("os_version") == std::string::npos);
  REQUIRE(std::holds_alternative<AssetSnapshotRequest>(parsed_asset));
  REQUIRE_FALSE(
      std::get<AssetSnapshotRequest>(parsed_asset).os_version.has_value());
  REQUIRE(std::get<AssetSnapshotRequest>(parsed_asset).arch == asset.arch);
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

  const auto parsed_status =
      zfleet::protocol::ParseStatusResponse(
          zfleet::protocol::SerializeStatusResponse(status));
  REQUIRE(std::holds_alternative<StatusResponse>(parsed_status));
  REQUIRE(std::get<StatusResponse>(parsed_status).status == "accepted");
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

  const auto error_json = zfleet::protocol::SerializeErrorResponse(error);
  const auto parsed_error = zfleet::protocol::ParseErrorResponse(error_json);

  REQUIRE(error_json.find("agent_id") == std::string::npos);
  REQUIRE(std::holds_alternative<ErrorResponse>(parsed_error));
  REQUIRE(std::get<ErrorResponse>(parsed_error).error_code ==
          ErrorCode::missing_required_field);
  REQUIRE_FALSE(std::get<ErrorResponse>(parsed_error).retryable);
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

TEST_CASE("task create request supports json round trip") {
  TaskCreateRequest request{
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

  const auto request_json =
      zfleet::protocol::SerializeTaskCreateRequest(request);
  const auto parsed_request =
      zfleet::protocol::ParseTaskCreateRequest(request_json);

  REQUIRE(std::holds_alternative<TaskCreateRequest>(parsed_request));
  REQUIRE(std::get<TaskCreateRequest>(parsed_request).request_id ==
          "task-create-1");
  REQUIRE(std::get<TaskCreateRequest>(parsed_request).task.task_id == "task-1");
}

TEST_CASE("task poll response omits task when idle") {
  TaskPollResponse response{
      .protocol_version = "v1",
      .request_id = "poll-1",
      .agent_id = "agent-1",
      .occurred_at = "2026-05-16T10:00:10Z",
      .task = std::nullopt,
      .status = TaskPollStatus::idle,
      .server_time = "2026-05-16T10:00:10Z",
  };

  const auto response_json =
      zfleet::protocol::SerializeTaskPollResponse(response);
  const auto parsed_response =
      zfleet::protocol::ParseTaskPollResponse(response_json);

  REQUIRE(response_json.find("\"task\"") == std::string::npos);
  REQUIRE(std::holds_alternative<TaskPollResponse>(parsed_response));
  REQUIRE_FALSE(std::get<TaskPollResponse>(parsed_response).task.has_value());
  REQUIRE(std::get<TaskPollResponse>(parsed_response).status ==
          TaskPollStatus::idle);
}

TEST_CASE("task result request supports success and failure shapes") {
  TaskResultRequest success{
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
      zfleet::protocol::SerializeTaskResultRequest(success);
  const auto parsed_success =
      zfleet::protocol::ParseTaskResultRequest(success_json);

  REQUIRE(success_json.find("\"result\"") != std::string::npos);
  REQUIRE(success_json.find("\"error\"") == std::string::npos);
  REQUIRE(std::holds_alternative<TaskResultRequest>(parsed_success));
  REQUIRE(std::get<TaskResultRequest>(parsed_success).status ==
          TaskExecutionStatus::succeeded);
  REQUIRE(std::get<TaskResultRequest>(parsed_success).result.has_value());
  REQUIRE_FALSE(std::get<TaskResultRequest>(parsed_success).error.has_value());
  REQUIRE(std::get<TaskResultRequest>(parsed_success).task_type ==
          TaskType::collect_basic_inventory);
  REQUIRE(std::holds_alternative<CollectBasicInventoryResult>(
      *std::get<TaskResultRequest>(parsed_success).result));
  REQUIRE(std::get<CollectBasicInventoryResult>(
              *std::get<TaskResultRequest>(parsed_success).result)
              .hostname == "devbox-01");

  TaskResultRequest failure{
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
      zfleet::protocol::SerializeTaskResultRequest(failure);
  const auto parsed_failure =
      zfleet::protocol::ParseTaskResultRequest(failure_json);

  REQUIRE(failure_json.find("\"result\"") == std::string::npos);
  REQUIRE(failure_json.find("\"error\"") != std::string::npos);
  REQUIRE(std::holds_alternative<TaskResultRequest>(parsed_failure));
  REQUIRE(std::get<TaskResultRequest>(parsed_failure).status ==
          TaskExecutionStatus::failed);
  REQUIRE_FALSE(std::get<TaskResultRequest>(parsed_failure).result.has_value());
  REQUIRE(std::get<TaskResultRequest>(parsed_failure).error.has_value());
  REQUIRE(std::get<TaskResultRequest>(parsed_failure).error->error_code ==
          ErrorCode::task_execution_failed);
}

TEST_CASE("task running request supports json round trip") {
  TaskRunningRequest request{
      .protocol_version = "v1",
      .request_id = "task-running-1",
      .task_id = "task-1",
      .agent_id = "agent-1",
      .task_type = TaskType::collect_basic_inventory,
      .occurred_at = "2026-05-17T10:00:10Z",
  };

  const auto request_json =
      zfleet::protocol::SerializeTaskRunningRequest(request);
  const auto parsed_request =
      zfleet::protocol::ParseTaskRunningRequest(request_json);

  REQUIRE(std::holds_alternative<TaskRunningRequest>(parsed_request));
  REQUIRE(std::get<TaskRunningRequest>(parsed_request).task_id == "task-1");
  REQUIRE(std::get<TaskRunningRequest>(parsed_request).task_type ==
          TaskType::collect_basic_inventory);
}
