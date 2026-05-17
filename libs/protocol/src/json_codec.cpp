#include "zfleet/protocol/json_codec.h"

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>
#include <utility>

namespace zfleet::protocol {

void to_json(nlohmann::json& j, const RegistrationRequest& request);
void from_json(const nlohmann::json& j, RegistrationRequest& request);
void to_json(nlohmann::json& j, const HeartbeatRequest& request);
void from_json(const nlohmann::json& j, HeartbeatRequest& request);
void to_json(nlohmann::json& j, const AssetSnapshotRequest& request);
void from_json(const nlohmann::json& j, AssetSnapshotRequest& request);
void to_json(nlohmann::json& j, const StatusResponse& response);
void from_json(const nlohmann::json& j, StatusResponse& response);
void to_json(nlohmann::json& j, const ErrorResponse& response);
void from_json(const nlohmann::json& j, ErrorResponse& response);
void to_json(nlohmann::json& j, const AuditEvent& event);
void from_json(const nlohmann::json& j, AuditEvent& event);
void to_json(nlohmann::json& j, const CollectBasicInventoryInput& input);
void from_json(const nlohmann::json& j, CollectBasicInventoryInput& input);
void to_json(nlohmann::json& j, const CollectBasicInventoryResult& result);
void from_json(const nlohmann::json& j, CollectBasicInventoryResult& result);
void to_json(nlohmann::json& j, const Task& task);
void from_json(const nlohmann::json& j, Task& task);
void to_json(nlohmann::json& j, const TaskCreateRequest& request);
void from_json(const nlohmann::json& j, TaskCreateRequest& request);
void to_json(nlohmann::json& j, const TaskPollResponse& response);
void from_json(const nlohmann::json& j, TaskPollResponse& response);
void to_json(nlohmann::json& j, const TaskError& error);
void from_json(const nlohmann::json& j, TaskError& error);
void to_json(nlohmann::json& j, const TaskRunningRequest& request);
void from_json(const nlohmann::json& j, TaskRunningRequest& request);
void to_json(nlohmann::json& j, const TaskResultRequest& request);
void from_json(const nlohmann::json& j, TaskResultRequest& request);

namespace {

using nlohmann::json;

template <typename T>
T required(const json& j, const char* key) {
  return j.at(key).get<T>();
}

template <typename T>
void assign_optional(const json& j, const char* key, std::optional<T>& out) {
  if (!j.contains(key) || j.at(key).is_null()) {
    out.reset();
    return;
  }

  out = j.at(key).get<T>();
}

ErrorCode parse_error_code(const std::string& code) {
  const auto parsed = ErrorCodeFromString(code);
  if (!parsed.has_value()) {
    throw std::invalid_argument("unknown error_code: " + code);
  }

  return *parsed;
}

AuditEventType parse_audit_event_type(const std::string& type) {
  const auto parsed = AuditEventTypeFromString(type);
  if (!parsed.has_value()) {
    throw std::invalid_argument("unknown event_type: " + type);
  }

  return *parsed;
}

TaskType parse_task_type(const std::string& type) {
  const auto parsed = TaskTypeFromString(type);
  if (!parsed.has_value()) {
    throw std::invalid_argument("unknown task_type: " + type);
  }

  return *parsed;
}

CapabilityLevel parse_capability_level(const std::string& level) {
  const auto parsed = CapabilityLevelFromString(level);
  if (!parsed.has_value()) {
    throw std::invalid_argument("unknown capability_level: " + level);
  }

  return *parsed;
}

TaskPollStatus parse_task_poll_status(const std::string& status) {
  const auto parsed = TaskPollStatusFromString(status);
  if (!parsed.has_value()) {
    throw std::invalid_argument("unknown task poll status: " + status);
  }

  return *parsed;
}

TaskExecutionStatus parse_task_execution_status(const std::string& status) {
  const auto parsed = TaskExecutionStatusFromString(status);
  if (!parsed.has_value()) {
    throw std::invalid_argument("unknown task execution status: " + status);
  }

  return *parsed;
}

TaskInput parse_task_input(TaskType type, const json& input) {
  switch (type) {
    case TaskType::collect_basic_inventory:
      return input.get<CollectBasicInventoryInput>();
  }

  throw std::invalid_argument("unknown task_type for input parsing");
}

json serialize_task_input(const TaskInput& input) {
  return std::visit([](const auto& value) { return json(value); }, input);
}

json serialize_task_result_data(const TaskResultData& result) {
  return std::visit([](const auto& value) { return json(value); }, result);
}

TaskResultData parse_task_result_data(TaskType type, const json& result) {
  switch (type) {
    case TaskType::collect_basic_inventory:
      return result.get<CollectBasicInventoryResult>();
  }

  throw std::invalid_argument("unknown task_type for result parsing");
}

JsonCodecError MakeCodecError(const nlohmann::json::parse_error& ex,
                              JsonCodecContext context = {}) {
  return JsonCodecError{
      .code = JsonCodecErrorCode::invalid_json,
      .message = ex.what(),
      .context = std::move(context),
  };
}

JsonCodecError MakeCodecError(const nlohmann::json::out_of_range& ex,
                              JsonCodecContext context = {}) {
  return JsonCodecError{
      .code = JsonCodecErrorCode::missing_required_field,
      .message = ex.what(),
      .context = std::move(context),
  };
}

JsonCodecError MakeCodecError(const nlohmann::json::type_error& ex,
                              JsonCodecContext context = {}) {
  return JsonCodecError{
      .code = JsonCodecErrorCode::invalid_field_type,
      .message = ex.what(),
      .context = std::move(context),
  };
}

JsonCodecError MakeCodecError(const std::invalid_argument& ex,
                              JsonCodecContext context = {}) {
  return JsonCodecError{
      .code = JsonCodecErrorCode::invalid_field_value,
      .message = ex.what(),
      .context = std::move(context),
  };
}

std::optional<std::string> ExtractStringField(const json& j,
                                              std::string_view key) {
  if (!j.is_object() || !j.contains(key) || !j.at(key).is_string()) {
    return std::nullopt;
  }
  return j.at(key).get<std::string>();
}

JsonCodecContext ExtractRequestContext(const json& j) {
  return JsonCodecContext{
      .request_id = ExtractStringField(j, "request_id"),
      .agent_id = ExtractStringField(j, "agent_id"),
  };
}

JsonCodecContext ExtractTaskCreateContext(const json& j) {
  JsonCodecContext context{.request_id = ExtractStringField(j, "request_id"),
                           .agent_id = std::nullopt};
  if (!j.is_object() || !j.contains("task") || !j.at("task").is_object()) {
    return context;
  }
  context.agent_id = ExtractStringField(j.at("task"), "agent_id");
  return context;
}

template <typename T, typename ParseFn, typename ContextFn>
JsonDecodeResult<T> ParseJson(std::string_view json_text,
                              ParseFn&& parse_fn,
                              ContextFn&& context_fn) {
  try {
    const auto parsed_json = json::parse(json_text);
    const auto context = context_fn(parsed_json);
    try {
      return parse_fn(parsed_json);
    } catch (const json::out_of_range& ex) {
      return MakeCodecError(ex, context);
    } catch (const json::type_error& ex) {
      return MakeCodecError(ex, context);
    } catch (const std::invalid_argument& ex) {
      return MakeCodecError(ex, context);
    }
  } catch (const json::parse_error& ex) {
    return MakeCodecError(ex);
  }
}

template <typename T, typename ParseFn>
JsonDecodeResult<T> ParseJson(std::string_view json_text, ParseFn&& parse_fn) {
  return ParseJson<T>(json_text, std::forward<ParseFn>(parse_fn),
                      [](const json&) { return JsonCodecContext{}; });
}

std::string DumpJson(const json& value) {
  return value.dump();
}

} // namespace

void to_json(json& j, const RegistrationRequest& request) {
  j = {
      {"protocol_version", request.protocol_version},
      {"request_id", request.request_id},
      {"agent_id", request.agent_id},
      {"occurred_at", request.occurred_at},
      {"agent_version", request.agent_version},
      {"hostname", request.hostname},
      {"os", request.os},
      {"arch", request.arch},
  };
}

void from_json(const json& j, RegistrationRequest& request) {
  request.protocol_version = required<std::string>(j, "protocol_version");
  request.request_id = required<std::string>(j, "request_id");
  request.agent_id = required<std::string>(j, "agent_id");
  request.occurred_at = required<std::string>(j, "occurred_at");
  request.agent_version = required<std::string>(j, "agent_version");
  request.hostname = required<std::string>(j, "hostname");
  request.os = required<std::string>(j, "os");
  request.arch = required<std::string>(j, "arch");
}

void to_json(json& j, const HeartbeatRequest& request) {
  j = {
      {"protocol_version", request.protocol_version},
      {"request_id", request.request_id},
      {"agent_id", request.agent_id},
      {"occurred_at", request.occurred_at},
      {"agent_version", request.agent_version},
  };
}

void from_json(const json& j, HeartbeatRequest& request) {
  request.protocol_version = required<std::string>(j, "protocol_version");
  request.request_id = required<std::string>(j, "request_id");
  request.agent_id = required<std::string>(j, "agent_id");
  request.occurred_at = required<std::string>(j, "occurred_at");
  request.agent_version = required<std::string>(j, "agent_version");
}

void to_json(json& j, const AssetSnapshotRequest& request) {
  j = {
      {"protocol_version", request.protocol_version},
      {"request_id", request.request_id},
      {"agent_id", request.agent_id},
      {"occurred_at", request.occurred_at},
      {"hostname", request.hostname},
      {"os", request.os},
      {"arch", request.arch},
      {"agent_version", request.agent_version},
  };

  if (request.os_version.has_value()) {
    j["os_version"] = *request.os_version;
  }
}

void from_json(const json& j, AssetSnapshotRequest& request) {
  request.protocol_version = required<std::string>(j, "protocol_version");
  request.request_id = required<std::string>(j, "request_id");
  request.agent_id = required<std::string>(j, "agent_id");
  request.occurred_at = required<std::string>(j, "occurred_at");
  request.hostname = required<std::string>(j, "hostname");
  request.os = required<std::string>(j, "os");
  assign_optional(j, "os_version", request.os_version);
  request.arch = required<std::string>(j, "arch");
  request.agent_version = required<std::string>(j, "agent_version");
}

void to_json(json& j, const StatusResponse& response) {
  j = {
      {"protocol_version", response.protocol_version},
      {"request_id", response.request_id},
      {"agent_id", response.agent_id},
      {"occurred_at", response.occurred_at},
      {"status", response.status},
      {"server_time", response.server_time},
  };
}

void from_json(const json& j, StatusResponse& response) {
  response.protocol_version = required<std::string>(j, "protocol_version");
  response.request_id = required<std::string>(j, "request_id");
  response.agent_id = required<std::string>(j, "agent_id");
  response.occurred_at = required<std::string>(j, "occurred_at");
  response.status = required<std::string>(j, "status");
  response.server_time = required<std::string>(j, "server_time");
}

void to_json(json& j, const ErrorResponse& response) {
  j = {
      {"protocol_version", response.protocol_version},
      {"request_id", response.request_id},
      {"occurred_at", response.occurred_at},
      {"error_code", ToString(response.error_code)},
      {"message", response.message},
      {"retryable", response.retryable},
  };

  if (response.agent_id.has_value()) {
    j["agent_id"] = *response.agent_id;
  }
}

void from_json(const json& j, ErrorResponse& response) {
  response.protocol_version = required<std::string>(j, "protocol_version");
  response.request_id = required<std::string>(j, "request_id");
  assign_optional(j, "agent_id", response.agent_id);
  response.occurred_at = required<std::string>(j, "occurred_at");
  response.error_code =
      parse_error_code(required<std::string>(j, "error_code"));
  response.message = required<std::string>(j, "message");
  response.retryable = required<bool>(j, "retryable");
}

void to_json(json& j, const AuditEvent& event) {
  j = {
      {"audit_id", event.audit_id},
      {"occurred_at", event.occurred_at},
      {"request_id", event.request_id},
      {"event_type", ToString(event.event_type)},
      {"result", event.result},
      {"payload_json", event.payload_json},
  };

  if (event.agent_id.has_value()) {
    j["agent_id"] = *event.agent_id;
  }
}

void from_json(const json& j, AuditEvent& event) {
  event.audit_id = required<std::string>(j, "audit_id");
  event.occurred_at = required<std::string>(j, "occurred_at");
  assign_optional(j, "agent_id", event.agent_id);
  event.request_id = required<std::string>(j, "request_id");
  event.event_type =
      parse_audit_event_type(required<std::string>(j, "event_type"));
  event.result = required<std::string>(j, "result");
  event.payload_json = required<std::string>(j, "payload_json");
}

void to_json(json& j, const CollectBasicInventoryInput&) {
  j = json::object();
}

void from_json(const json& j, CollectBasicInventoryInput& input) {
  if (!j.is_object()) {
    throw std::invalid_argument("collect_basic_inventory input must be object");
  }
  input = CollectBasicInventoryInput{};
}

void to_json(json& j, const CollectBasicInventoryResult& result) {
  j = {
      {"hostname", result.hostname},
      {"os", result.os},
      {"arch", result.arch},
      {"agent_version", result.agent_version},
  };
}

void from_json(const json& j, CollectBasicInventoryResult& result) {
  result.hostname = required<std::string>(j, "hostname");
  result.os = required<std::string>(j, "os");
  result.arch = required<std::string>(j, "arch");
  result.agent_version = required<std::string>(j, "agent_version");
}

void to_json(json& j, const Task& task) {
  j = {
      {"protocol_version", task.protocol_version},
      {"task_id", task.task_id},
      {"agent_id", task.agent_id},
      {"task_type", ToString(task.task_type)},
      {"capability_level", ToString(task.capability_level)},
      {"created_at", task.created_at},
      {"expires_at", task.expires_at},
      {"input", serialize_task_input(task.input)},
  };
}

void from_json(const json& j, Task& task) {
  task.protocol_version = required<std::string>(j, "protocol_version");
  task.task_id = required<std::string>(j, "task_id");
  task.agent_id = required<std::string>(j, "agent_id");
  task.task_type = parse_task_type(required<std::string>(j, "task_type"));
  task.capability_level =
      parse_capability_level(required<std::string>(j, "capability_level"));
  task.created_at = required<std::string>(j, "created_at");
  task.expires_at = required<std::string>(j, "expires_at");
  task.input = parse_task_input(task.task_type, required<json>(j, "input"));
}

void to_json(json& j, const TaskCreateRequest& request) {
  j = {
      {"protocol_version", request.protocol_version},
      {"request_id", request.request_id},
      {"occurred_at", request.occurred_at},
      {"task", request.task},
  };
}

void from_json(const json& j, TaskCreateRequest& request) {
  request.protocol_version = required<std::string>(j, "protocol_version");
  request.request_id = required<std::string>(j, "request_id");
  request.occurred_at = required<std::string>(j, "occurred_at");
  request.task = required<Task>(j, "task");
}

void to_json(json& j, const TaskPollResponse& response) {
  j = {
      {"protocol_version", response.protocol_version},
      {"request_id", response.request_id},
      {"agent_id", response.agent_id},
      {"occurred_at", response.occurred_at},
      {"status", ToString(response.status)},
      {"server_time", response.server_time},
  };

  if (response.task.has_value()) {
    j["task"] = *response.task;
  }
}

void from_json(const json& j, TaskPollResponse& response) {
  response.protocol_version = required<std::string>(j, "protocol_version");
  response.request_id = required<std::string>(j, "request_id");
  response.agent_id = required<std::string>(j, "agent_id");
  response.occurred_at = required<std::string>(j, "occurred_at");
  if (j.contains("task") && !j.at("task").is_null()) {
    response.task = j.at("task").get<Task>();
  } else {
    response.task.reset();
  }
  response.status =
      parse_task_poll_status(required<std::string>(j, "status"));
  response.server_time = required<std::string>(j, "server_time");
}

void to_json(json& j, const TaskError& error) {
  j = {
      {"error_code", ToString(error.error_code)},
      {"message", error.message},
      {"retryable", error.retryable},
  };
}

void from_json(const json& j, TaskError& error) {
  error.error_code = parse_error_code(required<std::string>(j, "error_code"));
  error.message = required<std::string>(j, "message");
  error.retryable = required<bool>(j, "retryable");
}

void to_json(json& j, const TaskRunningRequest& request) {
  j = {
      {"protocol_version", request.protocol_version},
      {"request_id", request.request_id},
      {"task_id", request.task_id},
      {"agent_id", request.agent_id},
      {"task_type", ToString(request.task_type)},
      {"occurred_at", request.occurred_at},
  };
}

void from_json(const json& j, TaskRunningRequest& request) {
  request.protocol_version = required<std::string>(j, "protocol_version");
  request.request_id = required<std::string>(j, "request_id");
  request.task_id = required<std::string>(j, "task_id");
  request.agent_id = required<std::string>(j, "agent_id");
  request.task_type = parse_task_type(required<std::string>(j, "task_type"));
  request.occurred_at = required<std::string>(j, "occurred_at");
}

void to_json(json& j, const TaskResultRequest& request) {
  j = {
      {"protocol_version", request.protocol_version},
      {"request_id", request.request_id},
      {"task_id", request.task_id},
      {"agent_id", request.agent_id},
      {"task_type", ToString(request.task_type)},
      {"occurred_at", request.occurred_at},
      {"status", ToString(request.status)},
  };

  if (request.result.has_value()) {
    j["result"] = serialize_task_result_data(*request.result);
  }
  if (request.error.has_value()) {
    j["error"] = *request.error;
  }
}

void from_json(const json& j, TaskResultRequest& request) {
  request.protocol_version = required<std::string>(j, "protocol_version");
  request.request_id = required<std::string>(j, "request_id");
  request.task_id = required<std::string>(j, "task_id");
  request.agent_id = required<std::string>(j, "agent_id");
  request.task_type = parse_task_type(required<std::string>(j, "task_type"));
  request.occurred_at = required<std::string>(j, "occurred_at");
  request.status =
      parse_task_execution_status(required<std::string>(j, "status"));
  if (j.contains("result") && !j.at("result").is_null()) {
    request.result = parse_task_result_data(request.task_type, j.at("result"));
  } else {
    request.result.reset();
  }
  if (j.contains("error") && !j.at("error").is_null()) {
    request.error = j.at("error").get<TaskError>();
  } else {
    request.error.reset();
  }
}

std::string SerializeRegistrationRequest(const RegistrationRequest& request) {
  return DumpJson(json(request));
}

JsonDecodeResult<RegistrationRequest> ParseRegistrationRequest(
    std::string_view json_text) {
  return ParseJson<RegistrationRequest>(
      json_text, [](const json& j) { return j.get<RegistrationRequest>(); },
      ExtractRequestContext);
}

std::string SerializeHeartbeatRequest(const HeartbeatRequest& request) {
  return DumpJson(json(request));
}

JsonDecodeResult<HeartbeatRequest> ParseHeartbeatRequest(
    std::string_view json_text) {
  return ParseJson<HeartbeatRequest>(
      json_text, [](const json& j) { return j.get<HeartbeatRequest>(); },
      ExtractRequestContext);
}

std::string SerializeAssetSnapshotRequest(
    const AssetSnapshotRequest& request) {
  return DumpJson(json(request));
}

JsonDecodeResult<AssetSnapshotRequest> ParseAssetSnapshotRequest(
    std::string_view json_text) {
  return ParseJson<AssetSnapshotRequest>(
      json_text, [](const json& j) { return j.get<AssetSnapshotRequest>(); },
      ExtractRequestContext);
}

std::string SerializeStatusResponse(const StatusResponse& response) {
  return DumpJson(json(response));
}

JsonDecodeResult<StatusResponse> ParseStatusResponse(std::string_view json_text) {
  return ParseJson<StatusResponse>(
      json_text, [](const json& j) { return j.get<StatusResponse>(); },
      ExtractRequestContext);
}

std::string SerializeErrorResponse(const ErrorResponse& response) {
  return DumpJson(json(response));
}

JsonDecodeResult<ErrorResponse> ParseErrorResponse(std::string_view json_text) {
  return ParseJson<ErrorResponse>(
      json_text, [](const json& j) { return j.get<ErrorResponse>(); },
      ExtractRequestContext);
}

std::string SerializeAuditEvent(const AuditEvent& event) {
  return DumpJson(json(event));
}

JsonDecodeResult<AuditEvent> ParseAuditEvent(std::string_view json_text) {
  return ParseJson<AuditEvent>(
      json_text, [](const json& j) { return j.get<AuditEvent>(); },
      ExtractRequestContext);
}

std::string SerializeCollectBasicInventoryInput(
    const CollectBasicInventoryInput& input) {
  return DumpJson(json(input));
}

JsonDecodeResult<CollectBasicInventoryInput> ParseCollectBasicInventoryInput(
    std::string_view json_text) {
  return ParseJson<CollectBasicInventoryInput>(
      json_text,
      [](const json& j) { return j.get<CollectBasicInventoryInput>(); });
}

std::string SerializeCollectBasicInventoryResult(
    const CollectBasicInventoryResult& result) {
  return DumpJson(json(result));
}

JsonDecodeResult<CollectBasicInventoryResult> ParseCollectBasicInventoryResult(
    std::string_view json_text) {
  return ParseJson<CollectBasicInventoryResult>(
      json_text,
      [](const json& j) { return j.get<CollectBasicInventoryResult>(); });
}

std::string SerializeTask(const Task& task) {
  return DumpJson(json(task));
}

JsonDecodeResult<Task> ParseTask(std::string_view json_text) {
  return ParseJson<Task>(
      json_text, [](const json& j) { return j.get<Task>(); },
      ExtractRequestContext);
}

std::string SerializeTaskCreateRequest(const TaskCreateRequest& request) {
  return DumpJson(json(request));
}

JsonDecodeResult<TaskCreateRequest> ParseTaskCreateRequest(
    std::string_view json_text) {
  return ParseJson<TaskCreateRequest>(
      json_text, [](const json& j) { return j.get<TaskCreateRequest>(); },
      ExtractTaskCreateContext);
}

std::string SerializeTaskPollResponse(const TaskPollResponse& response) {
  return DumpJson(json(response));
}

JsonDecodeResult<TaskPollResponse> ParseTaskPollResponse(
    std::string_view json_text) {
  return ParseJson<TaskPollResponse>(
      json_text, [](const json& j) { return j.get<TaskPollResponse>(); },
      ExtractRequestContext);
}

std::string SerializeTaskError(const TaskError& error) {
  return DumpJson(json(error));
}

JsonDecodeResult<TaskError> ParseTaskError(std::string_view json_text) {
  return ParseJson<TaskError>(json_text,
                              [](const json& j) { return j.get<TaskError>(); });
}

std::string SerializeTaskRunningRequest(const TaskRunningRequest& request) {
  return DumpJson(json(request));
}

JsonDecodeResult<TaskRunningRequest> ParseTaskRunningRequest(
    std::string_view json_text) {
  return ParseJson<TaskRunningRequest>(
      json_text, [](const json& j) { return j.get<TaskRunningRequest>(); },
      ExtractRequestContext);
}

std::string SerializeTaskResultRequest(const TaskResultRequest& request) {
  return DumpJson(json(request));
}

JsonDecodeResult<TaskResultRequest> ParseTaskResultRequest(
    std::string_view json_text) {
  return ParseJson<TaskResultRequest>(
      json_text, [](const json& j) { return j.get<TaskResultRequest>(); },
      ExtractRequestContext);
}

std::string SerializeTaskInput(const TaskInput& input) {
  return DumpJson(serialize_task_input(input));
}

JsonDecodeResult<TaskInput> ParseTaskInput(TaskType type,
                                           std::string_view json_text) {
  return ParseJson<TaskInput>(
      json_text, [type](const json& j) { return parse_task_input(type, j); });
}

std::string SerializeTaskResultData(const TaskResultData& result) {
  return DumpJson(serialize_task_result_data(result));
}

JsonDecodeResult<TaskResultData> ParseTaskResultData(TaskType type,
                                                     std::string_view json_text) {
  return ParseJson<TaskResultData>(
      json_text,
      [type](const json& j) { return parse_task_result_data(type, j); });
}

std::string SerializeAuditPayload(
    std::initializer_list<AuditPayloadField> fields) {
  json payload = json::object();
  for (const auto& field : fields) {
    std::visit(
        [&](const auto& value) {
          payload[std::string(field.key)] = value;
        },
        field.value);
  }
  return DumpJson(payload);
}

} // namespace zfleet::protocol
