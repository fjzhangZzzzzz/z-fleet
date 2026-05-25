#include "http2_control_service.h"

#include <cstdint>
#include <exception>
#include <optional>
#include <stdexcept>
#include <utility>
#include <variant>

#include "zfleet/core/log.h"
#include "zfleet/core/time.h"
#include "zfleet/core/uuid.h"
#include "zfleet/crypto/sha256.h"
#include "zfleet/protocol/json_codec.h"

namespace zfleet::server {
namespace {

namespace proto = zfleet::protocol::v1;

zfleet::core::log::Context EventLogger(std::string_view route,
                                       const proto::AgentEvent& event) {
  return zfleet::core::log::Component("server").With(
      {{"route", route},
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
    case proto::TASK_TYPE_PACKAGE_UPDATE:
      return zfleet::protocol::TaskType::package_update;
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
    case proto::ERROR_CODE_PACKAGE_NOT_FOUND:
      return zfleet::protocol::ErrorCode::package_not_found;
    case proto::ERROR_CODE_PACKAGE_RETIRED:
      return zfleet::protocol::ErrorCode::package_retired;
    case proto::ERROR_CODE_PLATFORM_ARCH_MISMATCH:
      return zfleet::protocol::ErrorCode::platform_arch_mismatch;
    case proto::ERROR_CODE_BUILD_TYPE_NOT_ALLOWED:
      return zfleet::protocol::ErrorCode::build_type_not_allowed;
    case proto::ERROR_CODE_INSTALLER_TOO_OLD:
      return zfleet::protocol::ErrorCode::installer_too_old;
    case proto::ERROR_CODE_DOWNLOAD_FAILED:
      return zfleet::protocol::ErrorCode::download_failed;
    case proto::ERROR_CODE_CHECKSUM_MISMATCH:
      return zfleet::protocol::ErrorCode::checksum_mismatch;
    case proto::ERROR_CODE_APPLY_FAILED:
      return zfleet::protocol::ErrorCode::apply_failed;
    case proto::ERROR_CODE_START_NEW_AGENT_FAILED:
      return zfleet::protocol::ErrorCode::start_new_agent_failed;
    case proto::ERROR_CODE_WAITING_RECONNECT_TIMEOUT:
      return zfleet::protocol::ErrorCode::waiting_reconnect_timeout;
    case proto::ERROR_CODE_AGENT_REPORTED_UNEXPECTED_VERSION:
      return zfleet::protocol::ErrorCode::agent_reported_unexpected_version;
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

template <typename Message>
std::string SerializeProto(const Message& message) {
  std::string bytes;
  if (!message.SerializeToString(&bytes)) {
    throw std::runtime_error("failed to serialize protobuf message");
  }
  return bytes;
}

}  // namespace

Http2ControlService::Http2ControlService(ServerStore* store) : store_(store) {}

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
    case proto::AgentEvent::kAssetSnapshot:
      return HandleAssetSnapshot(event);
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

  const zfleet::protocol::TaskRunning request{
      .protocol_version = event.protocol_version(),
      .request_id = event.message_id(),
      .task_id = running.task_id(),
      .agent_id = event.agent_id(),
      .task_type = *task_type,
      .occurred_at = event.occurred_at(),
  };

  try {
    const auto stored_task = store_->FindTaskById(request.task_id);
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
    if (stored_task->state != zfleet::protocol::TaskState::assigned) {
      return InvalidArgument("task is not assigned");
    }

    store_->MarkTaskRunning(request);
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
               "task running task_id={}", request.task_id);
    return Accepted("running");
  } catch (const std::exception& ex) {
    ZFLOG_ERROR(EventLogger("http2.control.task_running", event),
                "task running failed: {}", ex.what());
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

  zfleet::protocol::TaskResult request{
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

  std::optional<std::string> result_blob;
  std::optional<std::string> error_blob;
  if (*status == zfleet::protocol::TaskExecutionStatus::succeeded) {
    if (*task_type == zfleet::protocol::TaskType::collect_basic_inventory) {
      const zfleet::protocol::CollectBasicInventoryResult inventory{
          .hostname = result.collect_basic_inventory().hostname(),
          .os = result.collect_basic_inventory().os(),
          .arch = result.collect_basic_inventory().arch(),
          .agent_version = result.collect_basic_inventory().agent_version(),
      };
      request.result = inventory;
      result_blob = SerializeProto(result.collect_basic_inventory());
    } else {
      const zfleet::protocol::PackageUpdateResult update{
          .component = result.package_update().component(),
          .package_id = result.package_update().package_id(),
          .version = result.package_update().version(),
          .state = result.package_update().state(),
          .error_code = result.package_update().error_code(),
          .error_message = result.package_update().error_message(),
      };
      request.result = update;
      result_blob = SerializeProto(result.package_update());
    }
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
    error_blob = SerializeProto(result.error());
  }

  try {
    const auto stored_task = store_->FindTaskById(request.task_id);
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

    store_->RecordTaskResult(request, result_blob, error_blob);
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
               "task result stored task_id={} status={}", request.task_id,
               zfleet::protocol::ToString(request.status));
    return Accepted("accepted");
  } catch (const std::exception& ex) {
    ZFLOG_ERROR(EventLogger("http2.control.task_result", event),
                "task result failed: {}", ex.what());
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
  const zfleet::protocol::AgentRegistration request{
      .protocol_version = event.protocol_version(),
      .request_id = event.message_id(),
      .agent_id = event.agent_id(),
      .occurred_at = event.occurred_at(),
      .agent_version = registration.agent_version(),
      .hostname = registration.hostname(),
      .os = registration.os(),
      .arch = registration.arch(),
      .registration_token =
          registration.registration_token().empty()
              ? std::optional<std::string>{}
              : std::optional<std::string>{registration.registration_token()},
  };

  try {
    bool token_used = false;
    if (!store_->AgentExists(request.agent_id) &&
        request.registration_token.has_value()) {
      token_used = store_->ConsumeRegistrationToken(
          zfleet::crypto::Sha256BytesHex(*request.registration_token),
          request.os, request.arch, zfleet::core::NowUtcRfc3339());
      if (!token_used) {
        RecordAuditEvent(
            zfleet::protocol::AuditEventType::registration_token_rejected,
            request.request_id, request.agent_id, "error",
            zfleet::protocol::SerializeAuditPayload(
                {{"status", std::string("rejected")},
                 {"message", std::string("invalid or expired token")}}));
        return InvalidArgument("registration token is invalid or expired");
      }
    }
    if (token_used) {
      RecordAuditEvent(
          zfleet::protocol::AuditEventType::registration_token_used,
          request.request_id, request.agent_id, "success",
          zfleet::protocol::SerializeAuditPayload(
              {{"status", std::string("consumed")},
               {"platform", request.os},
               {"arch", request.arch}}));
    }
    const auto previous = store_->FindAgent(request.agent_id);
    store_->UpsertAgent(request);
    const auto current = store_->FindAgent(request.agent_id);
    if (previous.has_value() &&
        previous->upgrade_state ==
            std::optional<std::string>{"waiting_reconnect"} &&
        current.has_value() && current->upgrade_state.has_value() &&
        (*current->upgrade_state == "succeeded" ||
         *current->upgrade_state == "failed")) {
      RecordAuditEvent(
          zfleet::protocol::AuditEventType::agent_upgrade_confirmed,
          request.request_id, request.agent_id,
          *current->upgrade_state == "succeeded" ? "success" : "error",
          zfleet::protocol::SerializeAuditPayload(
              {{"state", *current->upgrade_state},
               {"agent_version", request.agent_version},
               {"error_code", current->last_upgrade_error.value_or("")}}));
    }
    store_->RecordAssetSnapshot(
        zfleet::protocol::AssetSnapshot{
            .protocol_version = request.protocol_version,
            .request_id = request.request_id,
            .agent_id = request.agent_id,
            .occurred_at = request.occurred_at,
            .hostname = request.hostname,
            .os = request.os,
            .arch = request.arch,
            .agent_version = request.agent_version,
        },
        event);
    RecordAuditEvent(zfleet::protocol::AuditEventType::agent_register,
                     request.request_id, request.agent_id, "success",
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
                "register failed: {}", ex.what());
    return InternalError();
  }
}

ControlEventResult Http2ControlService::HandleHeartbeat(
    const proto::AgentEvent& event) const {
  const auto& heartbeat = event.heartbeat();
  const zfleet::protocol::AgentHeartbeat request{
      .protocol_version = event.protocol_version(),
      .request_id = event.message_id(),
      .agent_id = event.agent_id(),
      .occurred_at = event.occurred_at(),
      .agent_version = heartbeat.agent_version(),
  };

  try {
    if (!store_->AgentExists(request.agent_id)) {
      return NotFound("agent not registered");
    }

    ZFLOG_TRACE(EventLogger("http2.control.heartbeat", event),
                "heartbeat accepted");
    return Accepted("ok");
  } catch (const std::exception& ex) {
    ZFLOG_ERROR(EventLogger("http2.control.heartbeat", event),
                "heartbeat failed: {}", ex.what());
    return InternalError();
  }
}

ControlEventResult Http2ControlService::HandleAssetSnapshot(
    const proto::AgentEvent& event) const {
  const auto& snapshot = event.asset_snapshot();
  const zfleet::protocol::AssetSnapshot request{
      .protocol_version = event.protocol_version(),
      .request_id = event.message_id(),
      .agent_id = event.agent_id(),
      .occurred_at = event.occurred_at(),
      .hostname = snapshot.hostname(),
      .os = snapshot.os(),
      .os_version = snapshot.os_version().empty()
                        ? std::optional<std::string>{}
                        : std::optional<std::string>{snapshot.os_version()},
      .arch = snapshot.arch(),
      .agent_version = snapshot.agent_version(),
      .applications = {snapshot.applications().begin(),
                       snapshot.applications().end()},
      .services = {snapshot.services().begin(), snapshot.services().end()},
  };

  try {
    if (!store_->AgentExists(request.agent_id)) {
      RecordAuditEvent(
          zfleet::protocol::AuditEventType::agent_asset_snapshot,
          request.request_id, request.agent_id, "error",
          zfleet::protocol::SerializeAuditPayload(
              {{"transport", std::string("http2")},
               {"error_code",
                std::string(zfleet::protocol::ToString(
                    zfleet::protocol::ErrorCode::agent_not_registered))},
               {"message", std::string("agent not registered")}}));
      return NotFound("agent not registered");
    }

    store_->RecordAssetSnapshot(request, event);
    RecordAuditEvent(zfleet::protocol::AuditEventType::agent_asset_snapshot,
                     request.request_id, request.agent_id, "success",
                     zfleet::protocol::SerializeAuditPayload(
                         {{"transport", std::string("http2")},
                          {"status", std::string("accepted")},
                          {"hostname", request.hostname},
                          {"os", request.os},
                          {"arch", request.arch},
                          {"agent_version", request.agent_version}}));
    ZFLOG_INFO(EventLogger("http2.control.asset_snapshot", event),
               "asset snapshot stored");
    return Accepted("accepted");
  } catch (const std::exception& ex) {
    ZFLOG_ERROR(EventLogger("http2.control.asset_snapshot", event),
                "asset snapshot failed: {}", ex.what());
    return InternalError();
  }
}

void Http2ControlService::RecordAuditEvent(
    zfleet::protocol::AuditEventType event_type, std::string request_id,
    std::string agent_id, std::string result, std::string payload_json) const {
  store_->RecordAuditEvent(zfleet::protocol::AuditEvent{
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

}  // namespace zfleet::server
