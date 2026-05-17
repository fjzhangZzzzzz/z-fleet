#include "http_handler.h"

#include "zfleet/core/time.h"
#include "zfleet/core/uuid.h"
#include "zfleet/protocol/json_codec.h"

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <variant>
#include <string_view>

namespace zfleet::server {
namespace {

using nlohmann::json;

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
                      const json& payload) {
  database->RecordAuditEvent(zfleet::protocol::AuditEvent{
      .audit_id = zfleet::core::GenerateUuid(),
      .occurred_at = zfleet::core::NowUtcRfc3339(),
      .agent_id = std::move(agent_id),
      .request_id = std::move(request_id),
      .event_type = event_type,
      .result = std::move(result),
      .payload_json = payload.dump(),
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

template <typename T>
T ParseBody(const std::string& body) {
  return json::parse(body).get<T>();
}

http::response<http::string_body> MakeJsonResponse(http::status status_code,
                                                   const json& body) {
  http::response<http::string_body> response{status_code, 11};
  response.set(http::field::content_type, "application/json");
  response.body() = body.dump();
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

std::string SerializeTaskResultData(
    const zfleet::protocol::TaskResultData& result) {
  return std::visit([](const auto& value) { return json(value).dump(); }, result);
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
  try {
    const auto parsed =
        ParseBody<zfleet::protocol::TaskCreateRequest>(request.body());
    if (parsed.protocol_version != zfleet::protocol::protocol_version() ||
        parsed.task.protocol_version != zfleet::protocol::protocol_version()) {
      return MakeErrorResponse(http::status::bad_request,
                               zfleet::protocol::AuditEventType::task_queued,
                               zfleet::protocol::ErrorCode::
                                   unsupported_protocol_version,
                               "unsupported protocol version", false,
                               parsed.request_id, parsed.task.agent_id);
    }

    database_->EnqueueTask(parsed.task);
    RecordAuditEvent(database_,
                     zfleet::protocol::AuditEventType::task_queued,
                     parsed.request_id,
                     parsed.task.agent_id,
                     "success",
                     json{{"http_status", 200},
                          {"task_id", parsed.task.task_id},
                          {"task_type", zfleet::protocol::ToString(parsed.task.task_type)},
                          {"capability_level",
                           zfleet::protocol::ToString(
                               parsed.task.capability_level)}});
    ZFLOG_INFO(RequestLogger("tasks.create", parsed.request_id,
                             parsed.task.agent_id),
               "task queued task_id={} task_type={}",
               parsed.task.task_id,
               zfleet::protocol::ToString(parsed.task.task_type));

    const auto now = zfleet::core::NowUtcRfc3339();
    return MakeStatusResponse(
        http::status::ok,
        zfleet::protocol::StatusResponse{
            .protocol_version = std::string(zfleet::protocol::protocol_version()),
            .request_id = parsed.request_id,
            .agent_id = parsed.task.agent_id,
            .occurred_at = now,
            .status = "accepted",
            .server_time = now,
        });
  } catch (const json::parse_error&) {
    return MakeErrorResponse(http::status::bad_request,
                             std::nullopt,
                             zfleet::protocol::ErrorCode::invalid_json,
                             "invalid json", false,
                             zfleet::core::GenerateUuid(), std::nullopt);
  } catch (const json::out_of_range&) {
    return MakeErrorResponse(http::status::bad_request,
                             std::nullopt,
                             zfleet::protocol::ErrorCode::missing_required_field,
                             "missing required field", false,
                             zfleet::core::GenerateUuid(), std::nullopt);
  } catch (const json::type_error&) {
    return MakeErrorResponse(http::status::bad_request,
                             std::nullopt,
                             zfleet::protocol::ErrorCode::invalid_field_type,
                             "invalid field type", false,
                             zfleet::core::GenerateUuid(), std::nullopt);
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
  try {
    const auto parsed =
        ParseBody<zfleet::protocol::RegistrationRequest>(request.body());
    if (parsed.protocol_version != zfleet::protocol::protocol_version()) {
      return MakeErrorResponse(http::status::bad_request,
                               zfleet::protocol::AuditEventType::agent_register,
                               zfleet::protocol::ErrorCode::
                                   unsupported_protocol_version,
                               "unsupported protocol version", false,
                               parsed.request_id, parsed.agent_id);
    }

    database_->UpsertAgent(parsed);
    RecordAuditEvent(database_, zfleet::protocol::AuditEventType::agent_register,
                     parsed.request_id, parsed.agent_id, "success",
                     json{{"http_status", 200},
                          {"status", "accepted"},
                          {"agent_version", parsed.agent_version},
                          {"hostname", parsed.hostname},
                          {"os", parsed.os},
                          {"arch", parsed.arch}});
    ZFLOG_INFO(RequestLogger("register", parsed.request_id, parsed.agent_id),
               "registration accepted");

    const auto now = zfleet::core::NowUtcRfc3339();
    return MakeStatusResponse(
        http::status::ok,
        zfleet::protocol::StatusResponse{
            .protocol_version = std::string(zfleet::protocol::protocol_version()),
            .request_id = parsed.request_id,
            .agent_id = parsed.agent_id,
            .occurred_at = now,
            .status = "accepted",
            .server_time = now,
        });
  } catch (const json::parse_error&) {
    return MakeErrorResponse(http::status::bad_request,
                             std::nullopt,
                             zfleet::protocol::ErrorCode::invalid_json,
                             "invalid json", false,
                             zfleet::core::GenerateUuid(), std::nullopt);
  } catch (const json::out_of_range&) {
    return MakeErrorResponse(http::status::bad_request,
                             std::nullopt,
                             zfleet::protocol::ErrorCode::missing_required_field,
                             "missing required field", false,
                             zfleet::core::GenerateUuid(), std::nullopt);
  } catch (const json::type_error&) {
    return MakeErrorResponse(http::status::bad_request,
                             std::nullopt,
                             zfleet::protocol::ErrorCode::invalid_field_type,
                             "invalid field type", false,
                             zfleet::core::GenerateUuid(), std::nullopt);
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
  try {
    const auto parsed = ParseBody<zfleet::protocol::TaskRunningRequest>(
        request.body());
    if (parsed.protocol_version != zfleet::protocol::protocol_version()) {
      return MakeErrorResponse(http::status::bad_request,
                               zfleet::protocol::AuditEventType::task_running,
                               zfleet::protocol::ErrorCode::
                                   unsupported_protocol_version,
                               "unsupported protocol version", false,
                               parsed.request_id, parsed.agent_id);
    }
    if (parsed.task_id != task_id) {
      return MakeErrorResponse(http::status::bad_request,
                               zfleet::protocol::AuditEventType::task_running,
                               zfleet::protocol::ErrorCode::task_result_invalid,
                               "path task_id does not match body task_id",
                               false, parsed.request_id, parsed.agent_id);
    }

    const auto stored_task = database_->FindTaskById(parsed.task_id);
    if (!stored_task.has_value()) {
      return MakeErrorResponse(http::status::not_found,
                               zfleet::protocol::AuditEventType::task_running,
                               zfleet::protocol::ErrorCode::task_not_found,
                               "task not found", false, parsed.request_id,
                               parsed.agent_id);
    }
    if (stored_task->task.agent_id != parsed.agent_id) {
      return MakeErrorResponse(http::status::bad_request,
                               zfleet::protocol::AuditEventType::task_running,
                               zfleet::protocol::ErrorCode::task_agent_mismatch,
                               "task agent does not match running agent",
                               false, parsed.request_id, parsed.agent_id);
    }
    if (stored_task->task.task_type != parsed.task_type) {
      return MakeErrorResponse(http::status::bad_request,
                               zfleet::protocol::AuditEventType::task_running,
                               zfleet::protocol::ErrorCode::
                                   unsupported_task_type,
                               "task type does not match stored task", false,
                               parsed.request_id, parsed.agent_id);
    }
    if (stored_task->state == zfleet::protocol::TaskState::succeeded ||
        stored_task->state == zfleet::protocol::TaskState::failed ||
        stored_task->state == zfleet::protocol::TaskState::expired) {
      return MakeErrorResponse(http::status::conflict,
                               zfleet::protocol::AuditEventType::task_running,
                               zfleet::protocol::ErrorCode::
                                   task_already_finished,
                               "task already finished", false, parsed.request_id,
                               parsed.agent_id);
    }

    database_->MarkTaskRunning(parsed);
    RecordAuditEvent(database_,
                     zfleet::protocol::AuditEventType::task_running,
                     parsed.request_id,
                     parsed.agent_id,
                     "success",
                     json{{"http_status", 200},
                          {"task_id", parsed.task_id},
                          {"task_type", zfleet::protocol::ToString(parsed.task_type)},
                          {"status", "running"}});
    ZFLOG_INFO(RequestLogger("tasks.running", parsed.request_id, parsed.agent_id),
               "task running task_id={}",
               parsed.task_id);

    const auto now = zfleet::core::NowUtcRfc3339();
    return MakeStatusResponse(
        http::status::ok,
        zfleet::protocol::StatusResponse{
            .protocol_version = std::string(zfleet::protocol::protocol_version()),
            .request_id = parsed.request_id,
            .agent_id = parsed.agent_id,
            .occurred_at = now,
            .status = "accepted",
            .server_time = now,
        });
  } catch (const json::parse_error&) {
    return MakeErrorResponse(http::status::bad_request,
                             std::nullopt,
                             zfleet::protocol::ErrorCode::invalid_json,
                             "invalid json", false,
                             zfleet::core::GenerateUuid(), std::nullopt);
  } catch (const json::out_of_range&) {
    return MakeErrorResponse(http::status::bad_request,
                             std::nullopt,
                             zfleet::protocol::ErrorCode::missing_required_field,
                             "missing required field", false,
                             zfleet::core::GenerateUuid(), std::nullopt);
  } catch (const json::type_error&) {
    return MakeErrorResponse(http::status::bad_request,
                             std::nullopt,
                             zfleet::protocol::ErrorCode::invalid_field_type,
                             "invalid field type", false,
                             zfleet::core::GenerateUuid(), std::nullopt);
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
  try {
    const auto parsed = ParseBody<zfleet::protocol::HeartbeatRequest>(
        request.body());
    if (parsed.protocol_version != zfleet::protocol::protocol_version()) {
      return MakeErrorResponse(http::status::bad_request,
                               zfleet::protocol::AuditEventType::agent_heartbeat,
                               zfleet::protocol::ErrorCode::
                                   unsupported_protocol_version,
                               "unsupported protocol version", false,
                               parsed.request_id, parsed.agent_id);
    }
    if (parsed.agent_id != agent_id) {
      return MakeErrorResponse(http::status::bad_request,
                               zfleet::protocol::AuditEventType::agent_heartbeat,
                               zfleet::protocol::ErrorCode::agent_id_mismatch,
                               "path agent_id does not match body agent_id",
                               false, parsed.request_id, parsed.agent_id);
    }
    if (!database_->AgentExists(parsed.agent_id)) {
      return MakeErrorResponse(http::status::not_found,
                               zfleet::protocol::AuditEventType::agent_heartbeat,
                               zfleet::protocol::ErrorCode::agent_not_registered,
                               "agent not registered", true, parsed.request_id,
                               parsed.agent_id);
    }

    database_->RecordHeartbeat(parsed, json(parsed).dump());
    RecordAuditEvent(database_,
                     zfleet::protocol::AuditEventType::agent_heartbeat,
                     parsed.request_id,
                     parsed.agent_id,
                     "success",
                     json{{"http_status", 200},
                          {"status", "ok"},
                          {"agent_version", parsed.agent_version}});
    ZFLOG_INFO(RequestLogger("heartbeat", parsed.request_id, parsed.agent_id),
               "heartbeat stored");

    const auto now = zfleet::core::NowUtcRfc3339();
    return MakeStatusResponse(
        http::status::ok,
        zfleet::protocol::StatusResponse{
            .protocol_version = std::string(zfleet::protocol::protocol_version()),
            .request_id = parsed.request_id,
            .agent_id = parsed.agent_id,
            .occurred_at = now,
            .status = "ok",
            .server_time = now,
        });
  } catch (const json::parse_error&) {
    return MakeErrorResponse(http::status::bad_request,
                             std::nullopt,
                             zfleet::protocol::ErrorCode::invalid_json,
                             "invalid json", false,
                             zfleet::core::GenerateUuid(), std::nullopt);
  } catch (const json::out_of_range&) {
    return MakeErrorResponse(http::status::bad_request,
                             std::nullopt,
                             zfleet::protocol::ErrorCode::missing_required_field,
                             "missing required field", false,
                             zfleet::core::GenerateUuid(), std::nullopt);
  } catch (const json::type_error&) {
    return MakeErrorResponse(http::status::bad_request,
                             std::nullopt,
                             zfleet::protocol::ErrorCode::invalid_field_type,
                             "invalid field type", false,
                             zfleet::core::GenerateUuid(), std::nullopt);
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
    RecordAuditEvent(database_,
                     zfleet::protocol::AuditEventType::task_assigned,
                     request_id,
                     std::string(agent_id),
                     "success",
                     json{{"http_status", 200},
                          {"task_id", task->task_id},
                          {"task_type", zfleet::protocol::ToString(task->task_type)},
                          {"status", "assigned"}});
    ZFLOG_INFO(RequestLogger("tasks.poll", request_id, agent_id),
               "task assigned task_id={}",
               task->task_id);
    return MakeJsonResponse(
        http::status::ok,
        json(zfleet::protocol::TaskPollResponse{
            .protocol_version = std::string(zfleet::protocol::protocol_version()),
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
      json(zfleet::protocol::TaskPollResponse{
          .protocol_version = std::string(zfleet::protocol::protocol_version()),
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
  try {
    const auto parsed =
        ParseBody<zfleet::protocol::AssetSnapshotRequest>(request.body());
    if (parsed.protocol_version != zfleet::protocol::protocol_version()) {
      return MakeErrorResponse(http::status::bad_request,
                               zfleet::protocol::AuditEventType::agent_asset_snapshot,
                               zfleet::protocol::ErrorCode::
                                   unsupported_protocol_version,
                               "unsupported protocol version", false,
                               parsed.request_id, parsed.agent_id);
    }
    if (parsed.agent_id != agent_id) {
      return MakeErrorResponse(http::status::bad_request,
                               zfleet::protocol::AuditEventType::agent_asset_snapshot,
                               zfleet::protocol::ErrorCode::agent_id_mismatch,
                               "path agent_id does not match body agent_id",
                               false, parsed.request_id, parsed.agent_id);
    }
    if (!database_->AgentExists(parsed.agent_id)) {
      return MakeErrorResponse(http::status::not_found,
                               zfleet::protocol::AuditEventType::agent_asset_snapshot,
                               zfleet::protocol::ErrorCode::agent_not_registered,
                               "agent not registered", true, parsed.request_id,
                               parsed.agent_id);
    }

    database_->RecordAssetSnapshot(parsed, json(parsed).dump());
    RecordAuditEvent(database_,
                     zfleet::protocol::AuditEventType::agent_asset_snapshot,
                     parsed.request_id,
                     parsed.agent_id,
                     "success",
                     json{{"http_status", 200},
                          {"status", "stored"},
                          {"hostname", parsed.hostname},
                          {"os", parsed.os},
                          {"arch", parsed.arch}});
    ZFLOG_INFO(RequestLogger("assets", parsed.request_id, parsed.agent_id),
               "asset snapshot stored");

    const auto now = zfleet::core::NowUtcRfc3339();
    return MakeStatusResponse(
        http::status::ok,
        zfleet::protocol::StatusResponse{
            .protocol_version = std::string(zfleet::protocol::protocol_version()),
            .request_id = parsed.request_id,
            .agent_id = parsed.agent_id,
            .occurred_at = now,
            .status = "stored",
            .server_time = now,
        });
  } catch (const json::parse_error&) {
    return MakeErrorResponse(http::status::bad_request,
                             std::nullopt,
                             zfleet::protocol::ErrorCode::invalid_json,
                             "invalid json", false,
                             zfleet::core::GenerateUuid(), std::nullopt);
  } catch (const json::out_of_range&) {
    return MakeErrorResponse(http::status::bad_request,
                             std::nullopt,
                             zfleet::protocol::ErrorCode::missing_required_field,
                             "missing required field", false,
                             zfleet::core::GenerateUuid(), std::nullopt);
  } catch (const json::type_error&) {
    return MakeErrorResponse(http::status::bad_request,
                             std::nullopt,
                             zfleet::protocol::ErrorCode::invalid_field_type,
                             "invalid field type", false,
                             zfleet::core::GenerateUuid(), std::nullopt);
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
  try {
    const auto parsed = ParseBody<zfleet::protocol::TaskResultRequest>(
        request.body());
    if (parsed.protocol_version != zfleet::protocol::protocol_version()) {
      return MakeErrorResponse(http::status::bad_request,
                               std::nullopt,
                               zfleet::protocol::ErrorCode::
                                   unsupported_protocol_version,
                               "unsupported protocol version", false,
                               parsed.request_id, parsed.agent_id);
    }
    if (parsed.task_id != task_id) {
      return MakeErrorResponse(http::status::bad_request,
                               std::nullopt,
                               zfleet::protocol::ErrorCode::task_result_invalid,
                               "path task_id does not match body task_id",
                               false, parsed.request_id, parsed.agent_id);
    }

    const auto stored_task = database_->FindTaskById(parsed.task_id);
    if (!stored_task.has_value()) {
      return MakeErrorResponse(http::status::not_found,
                               std::nullopt,
                               zfleet::protocol::ErrorCode::task_not_found,
                               "task not found", false, parsed.request_id,
                               parsed.agent_id);
    }
    if (stored_task->task.agent_id != parsed.agent_id) {
      return MakeErrorResponse(http::status::bad_request,
                               std::nullopt,
                               zfleet::protocol::ErrorCode::task_agent_mismatch,
                               "task agent does not match result agent",
                               false, parsed.request_id, parsed.agent_id);
    }
    if (stored_task->task.task_type != parsed.task_type) {
      return MakeErrorResponse(http::status::bad_request,
                               std::nullopt,
                               zfleet::protocol::ErrorCode::
                                   unsupported_task_type,
                               "task type does not match stored task", false,
                               parsed.request_id, parsed.agent_id);
    }
    if (stored_task->state == zfleet::protocol::TaskState::succeeded ||
        stored_task->state == zfleet::protocol::TaskState::failed ||
        stored_task->state == zfleet::protocol::TaskState::expired) {
      return MakeErrorResponse(http::status::conflict,
                               std::nullopt,
                               zfleet::protocol::ErrorCode::
                                   task_already_finished,
                               "task already finished", false, parsed.request_id,
                               parsed.agent_id);
    }

    const auto now = zfleet::core::NowUtcRfc3339();
    if (stored_task->task.expires_at <= now &&
        parsed.status != zfleet::protocol::TaskExecutionStatus::expired) {
      return MakeErrorResponse(http::status::conflict,
                               std::nullopt,
                               zfleet::protocol::ErrorCode::task_expired,
                               "task expired", false, parsed.request_id,
                               parsed.agent_id);
    }

    const auto result_json = parsed.result.has_value()
                                 ? std::optional<std::string>{
                                       SerializeTaskResultData(*parsed.result)}
                                 : std::nullopt;
    const auto error_json = parsed.error.has_value()
                                ? std::optional<std::string>{
                                      nlohmann::json(*parsed.error).dump()}
                                : std::nullopt;
    database_->RecordTaskResult(parsed, result_json, error_json);
    RecordAuditEvent(database_,
                     AuditEventForTaskTerminalStatus(parsed.status),
                     parsed.request_id,
                     parsed.agent_id,
                     "success",
                     json{{"http_status", 200},
                          {"task_id", parsed.task_id},
                          {"task_type", zfleet::protocol::ToString(parsed.task_type)},
                          {"status", zfleet::protocol::ToString(parsed.status)}});
    ZFLOG_INFO(RequestLogger("tasks.result", parsed.request_id, parsed.agent_id),
               "task result stored task_id={} status={}",
               parsed.task_id,
               zfleet::protocol::ToString(parsed.status));

    return MakeStatusResponse(
        http::status::ok,
        zfleet::protocol::StatusResponse{
            .protocol_version = std::string(zfleet::protocol::protocol_version()),
            .request_id = parsed.request_id,
            .agent_id = parsed.agent_id,
            .occurred_at = now,
            .status = "accepted",
            .server_time = now,
        });
  } catch (const json::parse_error&) {
    return MakeErrorResponse(http::status::bad_request,
                             std::nullopt,
                             zfleet::protocol::ErrorCode::invalid_json,
                             "invalid json", false,
                             zfleet::core::GenerateUuid(), std::nullopt);
  } catch (const json::out_of_range&) {
    return MakeErrorResponse(http::status::bad_request,
                             std::nullopt,
                             zfleet::protocol::ErrorCode::missing_required_field,
                             "missing required field", false,
                             zfleet::core::GenerateUuid(), std::nullopt);
  } catch (const json::type_error&) {
    return MakeErrorResponse(http::status::bad_request,
                             std::nullopt,
                             zfleet::protocol::ErrorCode::invalid_field_type,
                             "invalid field type", false,
                             zfleet::core::GenerateUuid(), std::nullopt);
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
  return MakeJsonResponse(status_code, json(response));
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
    RecordAuditEvent(database_,
                     *event_type,
                     response.request_id,
                     response.agent_id,
                     "failure",
                     json{{"http_status", static_cast<int>(status_code)},
                          {"error_code", zfleet::protocol::ToString(error_code)},
                          {"message", response.message},
                          {"retryable", response.retryable}});
  }
  return MakeJsonResponse(status_code, json(response));
}

} // namespace zfleet::server
