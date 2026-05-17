#include "http_handler.h"

#include "zfleet/core/time.h"
#include "zfleet/core/uuid.h"
#include "zfleet/protocol/json_codec.h"

#include <stdexcept>
#include <string_view>
#include <variant>

namespace zfleet::server {
namespace {

constexpr std::string_view kRegisterRoute = "/v1/agents/register";
constexpr std::string_view kTaskCreateRoute = "/v1/tasks";
constexpr std::string_view kHeartbeatSuffix = "/heartbeat";
constexpr std::string_view kAssetsSuffix = "/assets";
constexpr std::string_view kTaskPollSuffix = "/tasks/poll";
constexpr std::string_view kTaskResultPrefix = "/v1/tasks/";
constexpr std::string_view kTaskRunningSuffix = "/running";
constexpr std::string_view kTaskResultSuffix = "/result";

void RecordAuditEvent(ServerDatabase* database,
                      zfleet::protocol::AuditEventType event_type,
                      std::string request_id,
                      std::optional<std::string> agent_id,
                      std::string result,
                      std::string payload_json) {
  database->RecordAuditEvent(zfleet::protocol::AuditEvent{
      .audit_id = zfleet::core::GenerateUuid(),
      .occurred_at = zfleet::core::NowUtcRfc3339(),
      .agent_id = std::move(agent_id),
      .request_id = std::move(request_id),
      .event_type = event_type,
      .result = std::move(result),
      .payload_json = std::move(payload_json),
  });
}

std::optional<std::string_view> MatchAgentRoute(std::string_view target,
                                                std::string_view suffix) {
  constexpr std::string_view kPrefix = "/v1/agents/";
  if (!target.starts_with(kPrefix) || !target.ends_with(suffix) ||
      target.size() <= kPrefix.size() + suffix.size()) {
    return std::nullopt;
  }

  const auto agent_id_size = target.size() - kPrefix.size() - suffix.size();
  return target.substr(kPrefix.size(), agent_id_size);
}

std::optional<std::string_view> MatchTaskResultRoute(std::string_view target) {
  if (!target.starts_with(kTaskResultPrefix) ||
      !target.ends_with(kTaskResultSuffix) ||
      target.size() <= kTaskResultPrefix.size() + kTaskResultSuffix.size()) {
    return std::nullopt;
  }

  const auto task_id_size =
      target.size() - kTaskResultPrefix.size() - kTaskResultSuffix.size();
  return target.substr(kTaskResultPrefix.size(), task_id_size);
}

std::optional<std::string_view> MatchTaskRoute(std::string_view target,
                                               std::string_view suffix) {
  if (!target.starts_with(kTaskResultPrefix) || !target.ends_with(suffix) ||
      target.size() <= kTaskResultPrefix.size() + suffix.size()) {
    return std::nullopt;
  }

  const auto task_id_size =
      target.size() - kTaskResultPrefix.size() - suffix.size();
  return target.substr(kTaskResultPrefix.size(), task_id_size);
}

http::response<http::string_body> MakeJsonResponse(http::status status_code,
                                                   std::string body) {
  http::response<http::string_body> response{status_code, 11};
  response.set(http::field::content_type, "application/json");
  response.body() = std::move(body);
  response.prepare_payload();
  return response;
}

zfleet::core::log::Context RequestLogger(std::string_view route,
                                         std::string_view request_id,
                                         std::string_view agent_id = {}) {
  auto context = zfleet::core::log::Component("server").With({{"route", route},
                                                              {"request_id",
                                                               request_id}});
  if (!agent_id.empty()) {
    context = context.With({"agent_id", agent_id});
  }
  return context;
}

std::string RequestIdForGet(const http::request<http::string_body>& request) {
  constexpr std::string_view kRequestIdHeader = "X-Request-Id";
  if (const auto it = request.find(kRequestIdHeader); it != request.end()) {
    return std::string(it->value());
  }
  return zfleet::core::GenerateUuid();
}

zfleet::protocol::AuditEventType AuditEventForTaskTerminalStatus(
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

struct TaskCreateValidationError {
  http::status status_code;
  zfleet::protocol::ErrorCode error_code;
  std::string_view message;
};

std::optional<TaskCreateValidationError> ValidateTaskCreateRequest(
    const zfleet::protocol::TaskCreateRequest& request) {
  if (request.request_id.empty()) {
    return TaskCreateValidationError{
        .status_code = http::status::bad_request,
        .error_code = zfleet::protocol::ErrorCode::missing_required_field,
        .message = "request_id must not be empty",
    };
  }
  if (request.task.task_id.empty()) {
    return TaskCreateValidationError{
        .status_code = http::status::bad_request,
        .error_code = zfleet::protocol::ErrorCode::missing_required_field,
        .message = "task_id must not be empty",
    };
  }
  if (request.task.agent_id.empty()) {
    return TaskCreateValidationError{
        .status_code = http::status::bad_request,
        .error_code = zfleet::protocol::ErrorCode::missing_required_field,
        .message = "agent_id must not be empty",
    };
  }
  if (request.task.capability_level !=
      zfleet::protocol::CapabilityLevel::readonly) {
    return TaskCreateValidationError{
        .status_code = http::status::forbidden,
        .error_code = zfleet::protocol::ErrorCode::capability_not_allowed,
        .message = "capability level is not allowed by current policy",
    };
  }
  return std::nullopt;
}

std::optional<TaskCreateValidationError> ClassifyTaskCreateArgumentError(
    std::string_view message) {
  if (message.starts_with("unknown task_type:")) {
    return TaskCreateValidationError{
        .status_code = http::status::bad_request,
        .error_code = zfleet::protocol::ErrorCode::unsupported_task_type,
        .message = "unsupported task type",
    };
  }
  if (message.starts_with("unknown capability_level:")) {
    return TaskCreateValidationError{
        .status_code = http::status::bad_request,
        .error_code = zfleet::protocol::ErrorCode::invalid_field_type,
        .message = "invalid capability level",
    };
  }
  if (message == "collect_basic_inventory input must be object") {
    return TaskCreateValidationError{
        .status_code = http::status::bad_request,
        .error_code = zfleet::protocol::ErrorCode::invalid_field_type,
        .message = "invalid task input",
    };
  }
  return std::nullopt;
}

std::optional<std::string_view> ValidateTaskResultShape(
    const zfleet::protocol::TaskResultRequest& request) {
  switch (request.status) {
    case zfleet::protocol::TaskExecutionStatus::succeeded:
      if (!request.result.has_value()) {
        return "succeeded result must include result";
      }
      if (request.error.has_value()) {
        return "succeeded result must not include error";
      }
      return std::nullopt;
    case zfleet::protocol::TaskExecutionStatus::failed:
    case zfleet::protocol::TaskExecutionStatus::expired:
      if (!request.error.has_value()) {
        return "failed or expired result must include error";
      }
      if (!request.result.has_value() && !request.error.has_value()) {
        return "result and error must not both be empty";
      }
      return std::nullopt;
  }

  return std::nullopt;
}

zfleet::protocol::ErrorCode ErrorCodeForParseFailure(
    zfleet::protocol::JsonCodecErrorCode code) {
  switch (code) {
    case zfleet::protocol::JsonCodecErrorCode::invalid_json:
      return zfleet::protocol::ErrorCode::invalid_json;
    case zfleet::protocol::JsonCodecErrorCode::missing_required_field:
      return zfleet::protocol::ErrorCode::missing_required_field;
    case zfleet::protocol::JsonCodecErrorCode::invalid_field_type:
    case zfleet::protocol::JsonCodecErrorCode::invalid_field_value:
      return zfleet::protocol::ErrorCode::invalid_field_type;
  }

  return zfleet::protocol::ErrorCode::invalid_json;
}

std::string MessageForParseFailure(zfleet::protocol::JsonCodecErrorCode code) {
  switch (code) {
    case zfleet::protocol::JsonCodecErrorCode::invalid_json:
      return "invalid json";
    case zfleet::protocol::JsonCodecErrorCode::missing_required_field:
      return "missing required field";
    case zfleet::protocol::JsonCodecErrorCode::invalid_field_type:
    case zfleet::protocol::JsonCodecErrorCode::invalid_field_value:
      return "invalid field type";
  }

  return "invalid json";
}

template <typename T>
const T* GetDecodedValue(const zfleet::protocol::JsonDecodeResult<T>& result) {
  return std::get_if<T>(&result);
}

template <typename T>
const zfleet::protocol::JsonCodecError* GetDecodeError(
    const zfleet::protocol::JsonDecodeResult<T>& result) {
  return std::get_if<zfleet::protocol::JsonCodecError>(&result);
}

std::optional<std::string> RequestIdFromContext(
    const zfleet::protocol::JsonCodecContext& context) {
  return context.request_id;
}

std::string RequestIdOrGenerated(
    const zfleet::protocol::JsonCodecContext& context) {
  if (context.request_id.has_value()) {
    return *context.request_id;
  }
  return zfleet::core::GenerateUuid();
}

} // namespace

HttpHandler::HttpHandler(ServerDatabase* database) : database_(database) {}

http::response<http::string_body> HttpHandler::Handle(
    const http::request<http::string_body>& request) const {
  const auto target = std::string_view(request.target().data(),
                                       request.target().size());
  if (request.method() == http::verb::get) {
    if (const auto agent_id = MatchAgentRoute(target, kTaskPollSuffix);
        agent_id.has_value()) {
      return HandleTaskPoll(request, *agent_id);
    }
    return MakeErrorResponse(http::status::bad_request,
                             std::nullopt,
                             zfleet::protocol::ErrorCode::invalid_json,
                             "unsupported route", false,
                             RequestIdForGet(request), std::nullopt);
  }

  if (request.method() != http::verb::post) {
    return MakeErrorResponse(http::status::bad_request,
                             std::nullopt,
                             zfleet::protocol::ErrorCode::invalid_json,
                             "unsupported method", false,
                             zfleet::core::GenerateUuid(), std::nullopt);
  }

  if (target == kRegisterRoute) {
    return HandleRegister(request);
  }
  if (target == kTaskCreateRoute) {
    return HandleTaskCreate(request);
  }
  if (const auto agent_id = MatchAgentRoute(target, kHeartbeatSuffix);
      agent_id.has_value()) {
    return HandleHeartbeat(request, *agent_id);
  }
  if (const auto agent_id = MatchAgentRoute(target, kAssetsSuffix);
      agent_id.has_value()) {
    return HandleAssets(request, *agent_id);
  }
  if (const auto task_id = MatchTaskRoute(target, kTaskRunningSuffix);
      task_id.has_value()) {
    return HandleTaskRunning(request, *task_id);
  }
  if (const auto task_id = MatchTaskResultRoute(target); task_id.has_value()) {
    return HandleTaskResult(request, *task_id);
  }

  return MakeErrorResponse(http::status::bad_request,
                           std::nullopt,
                           zfleet::protocol::ErrorCode::invalid_json,
                           "unsupported route", false,
                           zfleet::core::GenerateUuid(), std::nullopt);
}

http::response<http::string_body> HttpHandler::HandleTaskCreate(
    const http::request<http::string_body>& request) const {
  const auto parsed = zfleet::protocol::ParseTaskCreateRequest(request.body());
  if (const auto* error = GetDecodeError(parsed)) {
    if (error->code == zfleet::protocol::JsonCodecErrorCode::invalid_field_value) {
      if (const auto validation_error =
              ClassifyTaskCreateArgumentError(error->message);
          validation_error.has_value()) {
        return MakeErrorResponse(validation_error->status_code,
                                 zfleet::protocol::AuditEventType::task_queued,
                                 validation_error->error_code,
                                 validation_error->message, false,
                                 RequestIdOrGenerated(error->context),
                                 error->context.agent_id);
      }
    }
    return MakeErrorResponse(http::status::bad_request,
                             std::nullopt,
                             ErrorCodeForParseFailure(error->code),
                             MessageForParseFailure(error->code), false,
                             RequestIdOrGenerated(error->context),
                             error->context.agent_id);
  }

  const auto& task_request = *GetDecodedValue(parsed);
  try {
    if (task_request.protocol_version != zfleet::protocol::protocol_version() ||
        task_request.task.protocol_version != zfleet::protocol::protocol_version()) {
      return MakeErrorResponse(http::status::bad_request,
                               zfleet::protocol::AuditEventType::task_queued,
                               zfleet::protocol::ErrorCode::
                                   unsupported_protocol_version,
                               "unsupported protocol version", false,
                               task_request.request_id,
                               task_request.task.agent_id);
    }
    if (const auto validation_error = ValidateTaskCreateRequest(task_request);
        validation_error.has_value()) {
      return MakeErrorResponse(validation_error->status_code,
                               zfleet::protocol::AuditEventType::task_queued,
                               validation_error->error_code,
                               validation_error->message, false,
                               task_request.request_id,
                               task_request.task.agent_id);
    }

    database_->EnqueueTask(task_request.task);
    RecordAuditEvent(
        database_, zfleet::protocol::AuditEventType::task_queued,
        task_request.request_id, task_request.task.agent_id, "success",
        zfleet::protocol::SerializeAuditPayload(
            {{"http_status", std::int64_t{200}},
             {"task_id", task_request.task.task_id},
             {"task_type",
              std::string(zfleet::protocol::ToString(task_request.task.task_type))},
             {"capability_level",
              std::string(zfleet::protocol::ToString(
                  task_request.task.capability_level))}}));
    ZFLOG_INFO(RequestLogger("tasks.create", task_request.request_id,
                             task_request.task.agent_id),
               "task queued task_id={} task_type={}",
               task_request.task.task_id,
               zfleet::protocol::ToString(task_request.task.task_type));

    const auto now = zfleet::core::NowUtcRfc3339();
    return MakeStatusResponse(
        http::status::ok,
        zfleet::protocol::StatusResponse{
            .protocol_version = std::string(zfleet::protocol::protocol_version()),
            .request_id = task_request.request_id,
            .agent_id = task_request.task.agent_id,
            .occurred_at = now,
            .status = "accepted",
            .server_time = now,
        });
  } catch (const std::exception& ex) {
    ZFLOG_ERROR(
        zfleet::core::log::Component("server").With({"route", "tasks.create"}),
        "task create failed: {}",
        ex.what());
    return MakeErrorResponse(http::status::internal_server_error,
                             std::nullopt,
                             zfleet::protocol::ErrorCode::internal_error,
                             "internal error", true,
                             zfleet::core::GenerateUuid(), std::nullopt);
  }
}

http::response<http::string_body> HttpHandler::HandleRegister(
    const http::request<http::string_body>& request) const {
  const auto parsed = zfleet::protocol::ParseRegistrationRequest(request.body());
  if (const auto* error = GetDecodeError(parsed)) {
    return MakeErrorResponse(http::status::bad_request,
                             std::nullopt,
                             ErrorCodeForParseFailure(error->code),
                             MessageForParseFailure(error->code), false,
                             RequestIdOrGenerated(error->context),
                             error->context.agent_id);
  }

  const auto& registration = *GetDecodedValue(parsed);
  try {
    if (registration.protocol_version != zfleet::protocol::protocol_version()) {
      return MakeErrorResponse(http::status::bad_request,
                               zfleet::protocol::AuditEventType::agent_register,
                               zfleet::protocol::ErrorCode::
                                   unsupported_protocol_version,
                               "unsupported protocol version", false,
                               registration.request_id, registration.agent_id);
    }

    database_->UpsertAgent(registration);
    RecordAuditEvent(
        database_, zfleet::protocol::AuditEventType::agent_register,
        registration.request_id, registration.agent_id, "success",
        zfleet::protocol::SerializeAuditPayload(
            {{"http_status", std::int64_t{200}},
             {"status", std::string("accepted")},
             {"agent_version", registration.agent_version},
             {"hostname", registration.hostname},
             {"os", registration.os},
             {"arch", registration.arch}}));
    ZFLOG_INFO(RequestLogger("register", registration.request_id,
                             registration.agent_id),
               "registration accepted");

    const auto now = zfleet::core::NowUtcRfc3339();
    return MakeStatusResponse(
        http::status::ok,
        zfleet::protocol::StatusResponse{
            .protocol_version = std::string(zfleet::protocol::protocol_version()),
            .request_id = registration.request_id,
            .agent_id = registration.agent_id,
            .occurred_at = now,
            .status = "accepted",
            .server_time = now,
        });
  } catch (const std::exception& ex) {
    ZFLOG_ERROR(zfleet::core::log::Component("server").With({"route", "register"}),
                "register failed: {}",
                ex.what());
    return MakeErrorResponse(http::status::internal_server_error,
                             std::nullopt,
                             zfleet::protocol::ErrorCode::internal_error,
                             "internal error", true,
                             zfleet::core::GenerateUuid(), std::nullopt);
  }
}

http::response<http::string_body> HttpHandler::HandleTaskRunning(
    const http::request<http::string_body>& request,
    std::string_view task_id) const {
  const auto parsed = zfleet::protocol::ParseTaskRunningRequest(request.body());
  if (const auto* error = GetDecodeError(parsed)) {
    return MakeErrorResponse(http::status::bad_request,
                             std::nullopt,
                             ErrorCodeForParseFailure(error->code),
                             MessageForParseFailure(error->code), false,
                             RequestIdOrGenerated(error->context),
                             error->context.agent_id);
  }

  const auto& running = *GetDecodedValue(parsed);
  try {
    if (running.protocol_version != zfleet::protocol::protocol_version()) {
      return MakeErrorResponse(http::status::bad_request,
                               zfleet::protocol::AuditEventType::task_running,
                               zfleet::protocol::ErrorCode::
                                   unsupported_protocol_version,
                               "unsupported protocol version", false,
                               running.request_id, running.agent_id);
    }
    if (running.task_id != task_id) {
      return MakeErrorResponse(http::status::bad_request,
                               zfleet::protocol::AuditEventType::task_running,
                               zfleet::protocol::ErrorCode::task_result_invalid,
                               "path task_id does not match body task_id",
                               false, running.request_id, running.agent_id);
    }

    const auto stored_task = database_->FindTaskById(running.task_id);
    if (!stored_task.has_value()) {
      return MakeErrorResponse(http::status::not_found,
                               zfleet::protocol::AuditEventType::task_running,
                               zfleet::protocol::ErrorCode::task_not_found,
                               "task not found", false, running.request_id,
                               running.agent_id);
    }
    if (stored_task->task.agent_id != running.agent_id) {
      return MakeErrorResponse(http::status::bad_request,
                               zfleet::protocol::AuditEventType::task_running,
                               zfleet::protocol::ErrorCode::task_agent_mismatch,
                               "task agent does not match running agent",
                               false, running.request_id, running.agent_id);
    }
    if (stored_task->task.task_type != running.task_type) {
      return MakeErrorResponse(http::status::bad_request,
                               zfleet::protocol::AuditEventType::task_running,
                               zfleet::protocol::ErrorCode::
                                   unsupported_task_type,
                               "task type does not match stored task", false,
                               running.request_id, running.agent_id);
    }
    if (stored_task->state == zfleet::protocol::TaskState::succeeded ||
        stored_task->state == zfleet::protocol::TaskState::failed ||
        stored_task->state == zfleet::protocol::TaskState::expired) {
      return MakeErrorResponse(http::status::conflict,
                               zfleet::protocol::AuditEventType::task_running,
                               zfleet::protocol::ErrorCode::
                                   task_already_finished,
                               "task already finished", false, running.request_id,
                               running.agent_id);
    }

    database_->MarkTaskRunning(running);
    RecordAuditEvent(
        database_, zfleet::protocol::AuditEventType::task_running,
        running.request_id, running.agent_id, "success",
        zfleet::protocol::SerializeAuditPayload(
            {{"http_status", std::int64_t{200}},
             {"task_id", running.task_id},
             {"task_type",
              std::string(zfleet::protocol::ToString(running.task_type))},
             {"status", std::string("running")}}));
    ZFLOG_INFO(RequestLogger("tasks.running", running.request_id,
                             running.agent_id),
               "task running task_id={}",
               running.task_id);

    const auto now = zfleet::core::NowUtcRfc3339();
    return MakeStatusResponse(
        http::status::ok,
        zfleet::protocol::StatusResponse{
            .protocol_version = std::string(zfleet::protocol::protocol_version()),
            .request_id = running.request_id,
            .agent_id = running.agent_id,
            .occurred_at = now,
            .status = "accepted",
            .server_time = now,
        });
  } catch (const std::exception& ex) {
    ZFLOG_ERROR(
        zfleet::core::log::Component("server").With({"route", "tasks.running"}),
        "task running failed: {}",
        ex.what());
    return MakeErrorResponse(http::status::internal_server_error,
                             std::nullopt,
                             zfleet::protocol::ErrorCode::internal_error,
                             "internal error", true,
                             zfleet::core::GenerateUuid(), std::nullopt);
  }
}

http::response<http::string_body> HttpHandler::HandleHeartbeat(
    const http::request<http::string_body>& request,
    std::string_view agent_id) const {
  const auto parsed = zfleet::protocol::ParseHeartbeatRequest(request.body());
  if (const auto* error = GetDecodeError(parsed)) {
    return MakeErrorResponse(http::status::bad_request,
                             std::nullopt,
                             ErrorCodeForParseFailure(error->code),
                             MessageForParseFailure(error->code), false,
                             RequestIdOrGenerated(error->context),
                             error->context.agent_id);
  }

  const auto& heartbeat = *GetDecodedValue(parsed);
  try {
    if (heartbeat.protocol_version != zfleet::protocol::protocol_version()) {
      return MakeErrorResponse(http::status::bad_request,
                               zfleet::protocol::AuditEventType::agent_heartbeat,
                               zfleet::protocol::ErrorCode::
                                   unsupported_protocol_version,
                               "unsupported protocol version", false,
                               heartbeat.request_id, heartbeat.agent_id);
    }
    if (heartbeat.agent_id != agent_id) {
      return MakeErrorResponse(http::status::bad_request,
                               zfleet::protocol::AuditEventType::agent_heartbeat,
                               zfleet::protocol::ErrorCode::agent_id_mismatch,
                               "path agent_id does not match body agent_id",
                               false, heartbeat.request_id, heartbeat.agent_id);
    }
    if (!database_->AgentExists(heartbeat.agent_id)) {
      return MakeErrorResponse(http::status::not_found,
                               zfleet::protocol::AuditEventType::agent_heartbeat,
                               zfleet::protocol::ErrorCode::agent_not_registered,
                               "agent not registered", true,
                               heartbeat.request_id, heartbeat.agent_id);
    }

    database_->RecordHeartbeat(
        heartbeat, zfleet::protocol::SerializeHeartbeatRequest(heartbeat));
    RecordAuditEvent(
        database_, zfleet::protocol::AuditEventType::agent_heartbeat,
        heartbeat.request_id, heartbeat.agent_id, "success",
        zfleet::protocol::SerializeAuditPayload(
            {{"http_status", std::int64_t{200}},
             {"status", std::string("ok")},
             {"agent_version", heartbeat.agent_version}}));
    ZFLOG_INFO(RequestLogger("heartbeat", heartbeat.request_id,
                             heartbeat.agent_id),
               "heartbeat stored");

    const auto now = zfleet::core::NowUtcRfc3339();
    return MakeStatusResponse(
        http::status::ok,
        zfleet::protocol::StatusResponse{
            .protocol_version = std::string(zfleet::protocol::protocol_version()),
            .request_id = heartbeat.request_id,
            .agent_id = heartbeat.agent_id,
            .occurred_at = now,
            .status = "ok",
            .server_time = now,
        });
  } catch (const std::exception& ex) {
    ZFLOG_ERROR(
        zfleet::core::log::Component("server").With({"route", "heartbeat"}),
        "heartbeat failed: {}",
        ex.what());
    return MakeErrorResponse(http::status::internal_server_error,
                             std::nullopt,
                             zfleet::protocol::ErrorCode::internal_error,
                             "internal error", true,
                             zfleet::core::GenerateUuid(), std::nullopt);
  }
}

http::response<http::string_body> HttpHandler::HandleTaskPoll(
    const http::request<http::string_body>& request,
    std::string_view agent_id) const {
  const auto request_id = RequestIdForGet(request);
  if (!database_->AgentExists(std::string(agent_id))) {
    return MakeErrorResponse(http::status::not_found,
                             std::nullopt,
                             zfleet::protocol::ErrorCode::agent_not_registered,
                             "agent not registered", true, request_id,
                             std::string(agent_id));
  }

  const auto now = zfleet::core::NowUtcRfc3339();
  if (const auto task =
          database_->ClaimNextTaskForAgent(std::string(agent_id), now);
      task.has_value()) {
    RecordAuditEvent(
        database_, zfleet::protocol::AuditEventType::task_assigned, request_id,
        std::string(agent_id), "success",
        zfleet::protocol::SerializeAuditPayload(
            {{"http_status", std::int64_t{200}},
             {"task_id", task->task_id},
             {"task_type",
              std::string(zfleet::protocol::ToString(task->task_type))},
             {"status", std::string("assigned")}}));
    ZFLOG_INFO(RequestLogger("tasks.poll", request_id, agent_id),
               "task assigned task_id={}",
               task->task_id);
    return MakeJsonResponse(
        http::status::ok,
        zfleet::protocol::SerializeTaskPollResponse(
            zfleet::protocol::TaskPollResponse{
                .protocol_version =
                    std::string(zfleet::protocol::protocol_version()),
                .request_id = request_id,
                .agent_id = std::string(agent_id),
                .occurred_at = now,
                .task = *task,
                .status = zfleet::protocol::TaskPollStatus::assigned,
                .server_time = now,
            }));
  }

  return MakeJsonResponse(
      http::status::ok,
      zfleet::protocol::SerializeTaskPollResponse(
          zfleet::protocol::TaskPollResponse{
              .protocol_version =
                  std::string(zfleet::protocol::protocol_version()),
              .request_id = request_id,
              .agent_id = std::string(agent_id),
              .occurred_at = now,
              .task = std::nullopt,
              .status = zfleet::protocol::TaskPollStatus::idle,
              .server_time = now,
          }));
}

http::response<http::string_body> HttpHandler::HandleAssets(
    const http::request<http::string_body>& request,
    std::string_view agent_id) const {
  const auto parsed = zfleet::protocol::ParseAssetSnapshotRequest(request.body());
  if (const auto* error = GetDecodeError(parsed)) {
    return MakeErrorResponse(http::status::bad_request,
                             std::nullopt,
                             ErrorCodeForParseFailure(error->code),
                             MessageForParseFailure(error->code), false,
                             RequestIdOrGenerated(error->context),
                             error->context.agent_id);
  }

  const auto& snapshot = *GetDecodedValue(parsed);
  try {
    if (snapshot.protocol_version != zfleet::protocol::protocol_version()) {
      return MakeErrorResponse(http::status::bad_request,
                               zfleet::protocol::AuditEventType::
                                   agent_asset_snapshot,
                               zfleet::protocol::ErrorCode::
                                   unsupported_protocol_version,
                               "unsupported protocol version", false,
                               snapshot.request_id, snapshot.agent_id);
    }
    if (snapshot.agent_id != agent_id) {
      return MakeErrorResponse(http::status::bad_request,
                               zfleet::protocol::AuditEventType::
                                   agent_asset_snapshot,
                               zfleet::protocol::ErrorCode::agent_id_mismatch,
                               "path agent_id does not match body agent_id",
                               false, snapshot.request_id, snapshot.agent_id);
    }
    if (!database_->AgentExists(snapshot.agent_id)) {
      return MakeErrorResponse(http::status::not_found,
                               zfleet::protocol::AuditEventType::
                                   agent_asset_snapshot,
                               zfleet::protocol::ErrorCode::agent_not_registered,
                               "agent not registered", true,
                               snapshot.request_id, snapshot.agent_id);
    }

    database_->RecordAssetSnapshot(
        snapshot, zfleet::protocol::SerializeAssetSnapshotRequest(snapshot));
    RecordAuditEvent(
        database_, zfleet::protocol::AuditEventType::agent_asset_snapshot,
        snapshot.request_id, snapshot.agent_id, "success",
        zfleet::protocol::SerializeAuditPayload(
            {{"http_status", std::int64_t{200}},
             {"status", std::string("stored")},
             {"hostname", snapshot.hostname},
             {"os", snapshot.os},
             {"arch", snapshot.arch}}));
    ZFLOG_INFO(RequestLogger("assets", snapshot.request_id, snapshot.agent_id),
               "asset snapshot stored");

    const auto now = zfleet::core::NowUtcRfc3339();
    return MakeStatusResponse(
        http::status::ok,
        zfleet::protocol::StatusResponse{
            .protocol_version = std::string(zfleet::protocol::protocol_version()),
            .request_id = snapshot.request_id,
            .agent_id = snapshot.agent_id,
            .occurred_at = now,
            .status = "stored",
            .server_time = now,
        });
  } catch (const std::exception& ex) {
    ZFLOG_ERROR(zfleet::core::log::Component("server").With({"route", "assets"}),
                "asset snapshot failed: {}",
                ex.what());
    return MakeErrorResponse(http::status::internal_server_error,
                             std::nullopt,
                             zfleet::protocol::ErrorCode::internal_error,
                             "internal error", true,
                             zfleet::core::GenerateUuid(), std::nullopt);
  }
}

http::response<http::string_body> HttpHandler::HandleTaskResult(
    const http::request<http::string_body>& request,
    std::string_view task_id) const {
  const auto parsed = zfleet::protocol::ParseTaskResultRequest(request.body());
  if (const auto* error = GetDecodeError(parsed)) {
    return MakeErrorResponse(http::status::bad_request,
                             std::nullopt,
                             ErrorCodeForParseFailure(error->code),
                             MessageForParseFailure(error->code), false,
                             RequestIdOrGenerated(error->context),
                             error->context.agent_id);
  }

  const auto& result_request = *GetDecodedValue(parsed);
  try {
    if (result_request.protocol_version != zfleet::protocol::protocol_version()) {
      return MakeErrorResponse(http::status::bad_request,
                               std::nullopt,
                               zfleet::protocol::ErrorCode::
                                   unsupported_protocol_version,
                               "unsupported protocol version", false,
                               result_request.request_id,
                               result_request.agent_id);
    }
    if (result_request.task_id != task_id) {
      return MakeErrorResponse(http::status::bad_request,
                               std::nullopt,
                               zfleet::protocol::ErrorCode::task_result_invalid,
                               "path task_id does not match body task_id",
                               false, result_request.request_id,
                               result_request.agent_id);
    }
    if (const auto shape_error = ValidateTaskResultShape(result_request);
        shape_error.has_value()) {
      return MakeErrorResponse(http::status::bad_request,
                               std::nullopt,
                               zfleet::protocol::ErrorCode::task_result_invalid,
                               *shape_error, false, result_request.request_id,
                               result_request.agent_id);
    }

    const auto stored_task = database_->FindTaskById(result_request.task_id);
    if (!stored_task.has_value()) {
      return MakeErrorResponse(http::status::not_found,
                               std::nullopt,
                               zfleet::protocol::ErrorCode::task_not_found,
                               "task not found", false,
                               result_request.request_id,
                               result_request.agent_id);
    }
    if (stored_task->task.agent_id != result_request.agent_id) {
      return MakeErrorResponse(http::status::bad_request,
                               std::nullopt,
                               zfleet::protocol::ErrorCode::task_agent_mismatch,
                               "task agent does not match result agent",
                               false, result_request.request_id,
                               result_request.agent_id);
    }
    if (stored_task->task.task_type != result_request.task_type) {
      return MakeErrorResponse(http::status::bad_request,
                               std::nullopt,
                               zfleet::protocol::ErrorCode::
                                   unsupported_task_type,
                               "task type does not match stored task", false,
                               result_request.request_id,
                               result_request.agent_id);
    }
    if (stored_task->state == zfleet::protocol::TaskState::succeeded ||
        stored_task->state == zfleet::protocol::TaskState::failed ||
        stored_task->state == zfleet::protocol::TaskState::expired) {
      return MakeErrorResponse(http::status::conflict,
                               std::nullopt,
                               zfleet::protocol::ErrorCode::
                                   task_already_finished,
                               "task already finished", false,
                               result_request.request_id,
                               result_request.agent_id);
    }

    const auto now = zfleet::core::NowUtcRfc3339();
    if (stored_task->task.expires_at <= now &&
        result_request.status != zfleet::protocol::TaskExecutionStatus::expired) {
      return MakeErrorResponse(http::status::conflict,
                               std::nullopt,
                               zfleet::protocol::ErrorCode::task_expired,
                               "task expired", false, result_request.request_id,
                               result_request.agent_id);
    }

    const auto result_json = result_request.result.has_value()
                                 ? std::optional<std::string>{
                                       zfleet::protocol::SerializeTaskResultData(
                                           *result_request.result)}
                                 : std::nullopt;
    const auto error_json = result_request.error.has_value()
                                ? std::optional<std::string>{
                                      zfleet::protocol::SerializeTaskError(
                                          *result_request.error)}
                                : std::nullopt;
    database_->RecordTaskResult(result_request, result_json, error_json);
    RecordAuditEvent(
        database_, AuditEventForTaskTerminalStatus(result_request.status),
        result_request.request_id, result_request.agent_id, "success",
        zfleet::protocol::SerializeAuditPayload(
            {{"http_status", std::int64_t{200}},
             {"task_id", result_request.task_id},
             {"task_type",
              std::string(zfleet::protocol::ToString(result_request.task_type))},
             {"status",
              std::string(zfleet::protocol::ToString(result_request.status))}}));
    ZFLOG_INFO(RequestLogger("tasks.result", result_request.request_id,
                             result_request.agent_id),
               "task result stored task_id={} status={}",
               result_request.task_id,
               zfleet::protocol::ToString(result_request.status));

    return MakeStatusResponse(
        http::status::ok,
        zfleet::protocol::StatusResponse{
            .protocol_version = std::string(zfleet::protocol::protocol_version()),
            .request_id = result_request.request_id,
            .agent_id = result_request.agent_id,
            .occurred_at = now,
            .status = "accepted",
            .server_time = now,
        });
  } catch (const std::exception& ex) {
    ZFLOG_ERROR(
        zfleet::core::log::Component("server").With({"route", "tasks.result"}),
        "task result failed: {}",
        ex.what());
    return MakeErrorResponse(http::status::internal_server_error,
                             std::nullopt,
                             zfleet::protocol::ErrorCode::internal_error,
                             "internal error", true,
                             zfleet::core::GenerateUuid(), std::nullopt);
  }
}

http::response<http::string_body> HttpHandler::MakeStatusResponse(
    http::status status_code,
    const zfleet::protocol::StatusResponse& response) const {
  return MakeJsonResponse(status_code,
                          zfleet::protocol::SerializeStatusResponse(response));
}

http::response<http::string_body> HttpHandler::MakeErrorResponse(
    http::status status_code,
    std::optional<zfleet::protocol::AuditEventType> event_type,
    zfleet::protocol::ErrorCode error_code,
    std::string_view message,
    bool retryable,
    std::string request_id,
    std::optional<std::string> agent_id) const {
  const auto response = zfleet::protocol::ErrorResponse{
      .protocol_version = std::string(zfleet::protocol::protocol_version()),
      .request_id = std::move(request_id),
      .agent_id = std::move(agent_id),
      .occurred_at = zfleet::core::NowUtcRfc3339(),
      .error_code = error_code,
      .message = std::string(message),
      .retryable = retryable,
  };
  if (event_type.has_value()) {
    RecordAuditEvent(
        database_, *event_type, response.request_id, response.agent_id, "failure",
        zfleet::protocol::SerializeAuditPayload(
            {{"http_status", std::int64_t{static_cast<int>(status_code)}},
             {"error_code", std::string(zfleet::protocol::ToString(error_code))},
             {"message", response.message},
             {"retryable", response.retryable}}));
  }
  return MakeJsonResponse(status_code,
                          zfleet::protocol::SerializeErrorResponse(response));
}

} // namespace zfleet::server
