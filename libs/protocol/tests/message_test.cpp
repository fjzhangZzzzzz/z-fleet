#include "zfleet/protocol/json_codec.h"
#include "zfleet/protocol/message.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

namespace {

using nlohmann::json;
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

  const auto task_json = json(task);
  const auto parsed_task = task_json.get<Task>();

  REQUIRE(parsed_task.task_type == TaskType::collect_basic_inventory);
  REQUIRE(parsed_task.capability_level == CapabilityLevel::readonly);
  REQUIRE(std::holds_alternative<CollectBasicInventoryInput>(parsed_task.input));
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

  const auto request_json = json(request);
  const auto parsed_request = request_json.get<TaskCreateRequest>();

  REQUIRE(parsed_request.request_id == "task-create-1");
  REQUIRE(parsed_request.task.task_id == "task-1");
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

  const auto response_json = json(response);
  const auto parsed_response = response_json.get<TaskPollResponse>();

  REQUIRE_FALSE(response_json.contains("task"));
  REQUIRE_FALSE(parsed_response.task.has_value());
  REQUIRE(parsed_response.status == TaskPollStatus::idle);
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

  const auto success_json = json(success);
  const auto parsed_success = success_json.get<TaskResultRequest>();

  REQUIRE(success_json.contains("result"));
  REQUIRE_FALSE(success_json.contains("error"));
  REQUIRE(parsed_success.status == TaskExecutionStatus::succeeded);
  REQUIRE(parsed_success.result.has_value());
  REQUIRE_FALSE(parsed_success.error.has_value());
  REQUIRE(parsed_success.task_type == TaskType::collect_basic_inventory);
  REQUIRE(std::holds_alternative<CollectBasicInventoryResult>(
      *parsed_success.result));
  REQUIRE(std::get<CollectBasicInventoryResult>(*parsed_success.result).hostname ==
          "devbox-01");

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

  const auto failure_json = json(failure);
  const auto parsed_failure = failure_json.get<TaskResultRequest>();

  REQUIRE_FALSE(failure_json.contains("result"));
  REQUIRE(failure_json.contains("error"));
  REQUIRE(parsed_failure.status == TaskExecutionStatus::failed);
  REQUIRE_FALSE(parsed_failure.result.has_value());
  REQUIRE(parsed_failure.error.has_value());
  REQUIRE(parsed_failure.error->error_code == ErrorCode::task_execution_failed);
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

  const auto request_json = json(request);
  const auto parsed_request = request_json.get<TaskRunningRequest>();

  REQUIRE(parsed_request.task_id == "task-1");
  REQUIRE(parsed_request.task_type == TaskType::collect_basic_inventory);
}
