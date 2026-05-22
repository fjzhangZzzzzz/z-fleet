#include "http2_control_service.h"

#include "zfleet/core/log.h"
#include "zfleet/core/time.h"
#include "zfleet/core/uuid.h"
#include "zfleet/protocol/json_codec.h"

#include <cstdint>
#include <exception>
#include <optional>
#include <utility>
#include <variant>

namespace zfleet::server {
namespace {

namespace proto = zfleet::protocol::v1;

zfleet::core::log::Context EventLogger(std::string_view route,
                                       const proto::AgentEvent& event) {
  return zfleet::core::log::Component("server")
      .With({{"route", route},
             {"request_id", event.message_id()},
             {"agent_id", event.agent_id()}});
}

ControlEventResult Accepted(std::string message) {
  return ControlEventResult{
      .status = ControlEventStatus::kAccepted,
      .message = std::move(message),
  };
}

ControlEventResult InvalidArgument(std::string message) {
  return ControlEventResult{
      .status = ControlEventStatus::kInvalidArgument,
      .message = std::move(message),
  };
}

ControlEventResult NotFound(std::string message) {
  return ControlEventResult{
      .status = ControlEventStatus::kNotFound,
      .message = std::move(message),
  };
}

ControlEventResult InternalError() {
  return ControlEventResult{
      .status = ControlEventStatus::kInternalError,
      .message = "internal error",
  };
}

std::optional<zfleet::protocol::TaskType> ToDomainTaskType(
    proto::TaskType type) {
  switch (type) {
    case proto::TASK_TYPE_COLLECT_BASIC_INVENTORY:
      return zfleet::protocol::TaskType::collect_basic_inventory;
    case proto::TASK_TYPE_UNSPECIFIED:
      return std::nullopt;
  }
  return std::nullopt;
}

std::optional<zfleet::protocol::TaskExecutionStatus> ToDomainStatus(
    proto::TaskExecutionStatus status) {
  switch (status) {
    case proto::TASK_EXECUTION_STATUS_SUCCEEDED:
      return zfleet::protocol::TaskExecutionStatus::succeeded;
    case proto::TASK_EXECUTION_STATUS_FAILED:
      return zfleet::protocol::TaskExecutionStatus::failed;
    case proto::TASK_EXECUTION_STATUS_EXPIRED:
      return zfleet::protocol::TaskExecutionStatus::expired;
    case proto::TASK_EXECUTION_STATUS_UNSPECIFIED:
      return std::nullopt;
  }
  return std::nullopt;
}

std::optional<zfleet::protocol::ErrorCode> ToDomainErrorCode(
    proto::ErrorCode code) {
  switch (code) {
    case proto::ERROR_CODE_AGENT_NOT_REGISTERED:
      return zfleet::protocol::ErrorCode::agent_not_registered;
    case proto::ERROR_CODE_INTERNAL_ERROR:
      return zfleet::protocol::ErrorCode::internal_error;
    case proto::ERROR_CODE_TASK_NOT_FOUND:
      return zfleet::protocol::ErrorCode::task_not_found;
    case proto::ERROR_CODE_TASK_ALREADY_FINISHED:
      return zfleet::protocol::ErrorCode::task_already_finished;
    case proto::ERROR_CODE_TASK_EXPIRED:
      return zfleet::protocol::ErrorCode::task_expired;
    case proto::ERROR_CODE_UNSUPPORTED_TASK_TYPE:
      return zfleet::protocol::ErrorCode::unsupported_task_type;
    case proto::ERROR_CODE_CAPABILITY_NOT_ALLOWED:
      return zfleet::protocol::ErrorCode::capability_not_allowed;
    case proto::ERROR_CODE_UNSUPPORTED_PROTOCOL_VERSION:
      return zfleet::protocol::ErrorCode::unsupported_protocol_version;
    case proto::ERROR_CODE_UNSPECIFIED:
      return std::nullopt;
  }
  return std::nullopt;
}

bool IsTerminal(zfleet::protocol::TaskState state) {
  return state == zfleet::protocol::TaskState::succeeded ||
         state == zfleet::protocol::TaskState::failed ||
         state == zfleet::protocol::TaskState::expired;
}

zfleet::protocol::AuditEventType AuditEventForTaskResult(
    zfleet::protocol::TaskExecutionStatus status) {
  switch (status) {
    case zfleet::protocol::TaskExecutionStatus::succeeded:
      return zfleet::protocol::AuditEventType::task_succeeded;
    case zfleet::protocol::TaskExecutionStatus::failed:
      return zfleet::protocol::AuditEventType::task_failed;
    case zfleet::protocol::TaskExecutionStatus::expired:
      return zfleet::protocol::AuditEventType::task_expired;
  }
  return zfleet::protocol::AuditEventType::task_failed;
}

} // namespace

Http2ControlService::Http2ControlService(ServerDatabase* database)
    : database_(database) {}

ControlEventResult Http2ControlService::HandleAgentEvent(
    const proto::AgentEvent& event) const {
  const auto validation = ValidateEnvelope(event);
  if (validation.status != ControlEventStatus::kAccepted) {
    return validation;
  }

  switch (event.payload_case()) {
    case proto::AgentEvent::kRegister:
      return HandleRegister(event);
    case proto::AgentEvent::kHeartbeat:
      return HandleHeartbeat(event);
    case proto::AgentEvent::kTaskRunning:
      return HandleTaskRunning(event);
    case proto::AgentEvent::kTaskResult:
      return HandleTaskResult(event);
    case proto::AgentEvent::PAYLOAD_NOT_SET:
      return InvalidArgument("event payload must be set");
    default:
      return InvalidArgument("unsupported agent event payload");
  }
}

ControlEventResult Http2ControlService::HandleTaskRunning(
    const proto::AgentEvent& event) const {
  const auto& running = event.task_running();
  const auto task_type = ToDomainTaskType(running.task_type());
  if (!task_type.has_value()) {
    return InvalidArgument("unsupported task type");
  }
  if (running.task_id().empty()) {
    return InvalidArgument("task_id must not be empty");
  }

  const zfleet::protocol::TaskRunningRequest request{
      .protocol_version = event.protocol_version(),
      .request_id = event.message_id(),
      .task_id = running.task_id(),
      .agent_id = event.agent_id(),
      .task_type = *task_type,
      .occurred_at = event.occurred_at(),
  };

  try {
    const auto stored_task = database_->FindTaskById(request.task_id);
    if (!stored_task.has_value()) {
      return NotFound("task not found");
    }
    if (stored_task->task.agent_id != request.agent_id) {
      return InvalidArgument("task agent does not match running agent");
    }
    if (stored_task->task.task_type != request.task_type) {
      return InvalidArgument("task type does not match stored task");
    }
    if (IsTerminal(stored_task->state)) {
      return InvalidArgument("task already finished");
    }

    database_->MarkTaskRunning(request);
    RecordAuditEvent(
        zfleet::protocol::AuditEventType::task_running, request.request_id,
        request.agent_id, "success",
        zfleet::protocol::SerializeAuditPayload(
            {{"transport", std::string("http2")},
             {"task_id", request.task_id},
             {"task_type",
              std::string(zfleet::protocol::ToString(request.task_type))},
             {"status", std::string("running")}}));
    ZFLOG_INFO(EventLogger("http2.control.task_running", event),
               "task running task_id={}",
               request.task_id);
    return Accepted("running");
  } catch (const std::exception& ex) {
    ZFLOG_ERROR(EventLogger("http2.control.task_running", event),
                "task running failed: {}",
                ex.what());
    return InternalError();
  }
}

ControlEventResult Http2ControlService::HandleTaskResult(
    const proto::AgentEvent& event) const {
  const auto& result = event.task_result();
  const auto task_type = ToDomainTaskType(result.task_type());
  const auto status = ToDomainStatus(result.status());
  if (!task_type.has_value()) {
    return InvalidArgument("unsupported task type");
  }
  if (!status.has_value()) {
    return InvalidArgument("unsupported task result status");
  }
  if (result.task_id().empty()) {
    return InvalidArgument("task_id must not be empty");
  }

  zfleet::protocol::TaskResultRequest request{
      .protocol_version = event.protocol_version(),
      .request_id = event.message_id(),
      .task_id = result.task_id(),
      .agent_id = event.agent_id(),
      .task_type = *task_type,
      .occurred_at = event.occurred_at(),
      .status = *status,
      .result = std::nullopt,
      .error = std::nullopt,
  };

  std::optional<std::string> result_json;
  std::optional<std::string> error_json;
  if (*status == zfleet::protocol::TaskExecutionStatus::succeeded) {
    const zfleet::protocol::CollectBasicInventoryResult inventory{
        .hostname = result.collect_basic_inventory().hostname(),
        .os = result.collect_basic_inventory().os(),
        .arch = result.collect_basic_inventory().arch(),
        .agent_version = result.collect_basic_inventory().agent_version(),
    };
    request.result = inventory;
    result_json = zfleet::protocol::SerializeCollectBasicInventoryResult(
        inventory);
  } else {
    const auto error_code = ToDomainErrorCode(result.error().code());
    if (!error_code.has_value()) {
      return InvalidArgument("task error code must be set");
    }
    const zfleet::protocol::TaskError error{
        .error_code = *error_code,
        .message = result.error().message(),
        .retryable = result.error().retryable(),
    };
    request.error = error;
    error_json = zfleet::protocol::SerializeTaskError(error);
  }

  try {
    const auto stored_task = database_->FindTaskById(request.task_id);
    if (!stored_task.has_value()) {
      return NotFound("task not found");
    }
    if (stored_task->task.agent_id != request.agent_id) {
      return InvalidArgument("task agent does not match result agent");
    }
    if (stored_task->task.task_type != request.task_type) {
      return InvalidArgument("task type does not match stored task");
    }
    if (IsTerminal(stored_task->state)) {
      return InvalidArgument("task already finished");
    }

    database_->RecordTaskResult(request, result_json, error_json);
    RecordAuditEvent(
        AuditEventForTaskResult(request.status), request.request_id,
        request.agent_id, "success",
        zfleet::protocol::SerializeAuditPayload(
            {{"transport", std::string("http2")},
             {"task_id", request.task_id},
             {"task_type",
              std::string(zfleet::protocol::ToString(request.task_type))},
             {"status",
              std::string(zfleet::protocol::ToString(request.status))}}));
    ZFLOG_INFO(EventLogger("http2.control.task_result", event),
               "task result stored task_id={} status={}",
               request.task_id,
               zfleet::protocol::ToString(request.status));
    return Accepted("accepted");
  } catch (const std::exception& ex) {
    ZFLOG_ERROR(EventLogger("http2.control.task_result", event),
                "task result failed: {}",
                ex.what());
    return InternalError();
  }
}

ControlEventResult Http2ControlService::ValidateEnvelope(
    const proto::AgentEvent& event) const {
  if (event.protocol_version() != zfleet::protocol::protocol_version()) {
    return InvalidArgument("unsupported protocol version");
  }
  if (event.message_id().empty()) {
    return InvalidArgument("message_id must not be empty");
  }
  if (event.agent_id().empty()) {
    return InvalidArgument("agent_id must not be empty");
  }
  if (event.occurred_at().empty()) {
    return InvalidArgument("occurred_at must not be empty");
  }
  return Accepted("accepted");
}

ControlEventResult Http2ControlService::HandleRegister(
    const proto::AgentEvent& event) const {
  const auto& registration = event.register_();
  const zfleet::protocol::RegistrationRequest request{
      .protocol_version = event.protocol_version(),
      .request_id = event.message_id(),
      .agent_id = event.agent_id(),
      .occurred_at = event.occurred_at(),
      .agent_version = registration.agent_version(),
      .hostname = registration.hostname(),
      .os = registration.os(),
      .arch = registration.arch(),
  };

  try {
    database_->UpsertAgent(request);
    RecordAuditEvent(
        zfleet::protocol::AuditEventType::agent_register, request.request_id,
        request.agent_id, "success",
        zfleet::protocol::SerializeAuditPayload(
            {{"transport", std::string("http2")},
             {"status", std::string("accepted")},
             {"agent_version", request.agent_version},
             {"hostname", request.hostname},
             {"os", request.os},
             {"arch", request.arch}}));
    ZFLOG_INFO(EventLogger("http2.control.register", event),
               "registration accepted");
    return Accepted("accepted");
  } catch (const std::exception& ex) {
    ZFLOG_ERROR(EventLogger("http2.control.register", event),
                "register failed: {}",
                ex.what());
    return InternalError();
  }
}

ControlEventResult Http2ControlService::HandleHeartbeat(
    const proto::AgentEvent& event) const {
  const auto& heartbeat = event.heartbeat();
  const zfleet::protocol::HeartbeatRequest request{
      .protocol_version = event.protocol_version(),
      .request_id = event.message_id(),
      .agent_id = event.agent_id(),
      .occurred_at = event.occurred_at(),
      .agent_version = heartbeat.agent_version(),
  };

  try {
    if (!database_->AgentExists(request.agent_id)) {
      RecordAuditEvent(
          zfleet::protocol::AuditEventType::agent_heartbeat,
          request.request_id, request.agent_id, "error",
          zfleet::protocol::SerializeAuditPayload(
              {{"transport", std::string("http2")},
               {"error_code",
                std::string(zfleet::protocol::ToString(
                    zfleet::protocol::ErrorCode::agent_not_registered))},
               {"message", std::string("agent not registered")}}));
      return NotFound("agent not registered");
    }

    database_->RecordHeartbeat(
        request, zfleet::protocol::SerializeHeartbeatRequest(request));
    RecordAuditEvent(
        zfleet::protocol::AuditEventType::agent_heartbeat, request.request_id,
        request.agent_id, "success",
        zfleet::protocol::SerializeAuditPayload(
            {{"transport", std::string("http2")},
             {"status", std::string("ok")},
             {"agent_version", request.agent_version}}));
    ZFLOG_INFO(EventLogger("http2.control.heartbeat", event),
               "heartbeat stored");
    return Accepted("ok");
  } catch (const std::exception& ex) {
    ZFLOG_ERROR(EventLogger("http2.control.heartbeat", event),
                "heartbeat failed: {}",
                ex.what());
    return InternalError();
  }
}

void Http2ControlService::RecordAuditEvent(
    zfleet::protocol::AuditEventType event_type,
    std::string request_id,
    std::string agent_id,
    std::string result,
    std::string payload_json) const {
  database_->RecordAuditEvent(zfleet::protocol::AuditEvent{
      .audit_id = zfleet::core::GenerateUuid(),
      .occurred_at = zfleet::core::NowUtcRfc3339(),
      .agent_id = std::move(agent_id),
      .request_id = std::move(request_id),
      .event_type = event_type,
      .result = std::move(result),
      .payload_json = std::move(payload_json),
  });
}

std::string_view ToString(ControlEventStatus status) noexcept {
  switch (status) {
    case ControlEventStatus::kAccepted:
      return "accepted";
    case ControlEventStatus::kInvalidArgument:
      return "invalid_argument";
    case ControlEventStatus::kNotFound:
      return "not_found";
    case ControlEventStatus::kInternalError:
      return "internal_error";
  }

  return "internal_error";
}

} // namespace zfleet::server
