#include "control_service.h"

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
#include "zfleet/protocol/control_codec.h"
#include "zfleet/protocol/json_codec.h"

namespace zfleet::server {
namespace {

zfleet::core::log::Context EventLogger(std::string_view route,
                                       const zfleet::protocol::AgentEvent& event) {
  return zfleet::core::log::Component("server").With(
      {{"route", route},
       {"request_id",
        std::string(zfleet::protocol::AgentEventRequestId(event))},
       {"agent_id", std::string(zfleet::protocol::AgentEventAgentId(event))}});
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

}  // namespace

ControlService::ControlService(ServerStore* store) : store_(store) {}

ControlEventResult ControlService::HandleAgentEvent(
    const zfleet::protocol::AgentEvent& event) const {
  const auto validation = ValidateEnvelope(event);
  if (validation.status != ControlEventStatus::kAccepted) {
    return validation;
  }

  return std::visit(
      [&](const auto& payload) {
        using Payload = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<Payload,
                                     zfleet::protocol::AgentRegistration>) {
          return HandleRegister(payload, event);
        } else if constexpr (std::is_same_v<
                                 Payload, zfleet::protocol::AgentHeartbeat>) {
          return HandleHeartbeat(payload, event);
        } else if constexpr (std::is_same_v<
                                 Payload, zfleet::protocol::AssetSnapshot>) {
          return HandleAssetSnapshot(payload, event);
        } else if constexpr (std::is_same_v<Payload,
                                            zfleet::protocol::TaskRunning>) {
          return HandleTaskRunning(payload, event);
        } else {
          return HandleTaskResult(payload, event);
        }
      },
      event.payload);
}

ControlEventResult ControlService::HandleTaskRunning(
    const zfleet::protocol::TaskRunning& request,
    const zfleet::protocol::AgentEvent& event) const {
  if (request.task_id.empty()) {
    return InvalidArgument("task_id must not be empty");
  }

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
    if (zfleet::protocol::IsTerminalTaskState(stored_task->state)) {
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

ControlEventResult ControlService::HandleTaskResult(
    const zfleet::protocol::TaskResult& request,
    const zfleet::protocol::AgentEvent& event) const {
  if (request.task_id.empty()) {
    return InvalidArgument("task_id must not be empty");
  }

  std::optional<std::string> result_blob;
  std::optional<std::string> error_blob;
  if (request.status == zfleet::protocol::TaskExecutionStatus::succeeded) {
    if (!request.result.has_value()) {
      return InvalidArgument("task result payload must be set");
    }
    result_blob = zfleet::protocol::SerializeTaskResultDataBlob(
        *request.result);
  } else {
    if (!request.error.has_value()) {
      return InvalidArgument("task error code must be set");
    }
    error_blob = zfleet::protocol::SerializeTaskErrorBlob(*request.error);
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
    if (zfleet::protocol::IsTerminalTaskState(stored_task->state)) {
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

ControlEventResult ControlService::ValidateEnvelope(
    const zfleet::protocol::AgentEvent& event) const {
  if (zfleet::protocol::AgentEventProtocolVersion(event) !=
      zfleet::protocol::protocol_version()) {
    return InvalidArgument("unsupported protocol version");
  }
  if (zfleet::protocol::AgentEventRequestId(event).empty()) {
    return InvalidArgument("message_id must not be empty");
  }
  if (zfleet::protocol::AgentEventAgentId(event).empty()) {
    return InvalidArgument("agent_id must not be empty");
  }
  if (zfleet::protocol::AgentEventOccurredAt(event).empty()) {
    return InvalidArgument("occurred_at must not be empty");
  }
  return Accepted("accepted");
}

ControlEventResult ControlService::HandleRegister(
    const zfleet::protocol::AgentRegistration& request,
    const zfleet::protocol::AgentEvent& event) const {
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

ControlEventResult ControlService::HandleHeartbeat(
    const zfleet::protocol::AgentHeartbeat& request,
    const zfleet::protocol::AgentEvent& event) const {
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

ControlEventResult ControlService::HandleAssetSnapshot(
    const zfleet::protocol::AssetSnapshot& request,
    const zfleet::protocol::AgentEvent& event) const {
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

void ControlService::RecordAuditEvent(
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
