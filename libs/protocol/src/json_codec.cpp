#include "zfleet/protocol/json_codec.h"

#include <stdexcept>
#include <string>

namespace zfleet::protocol {
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

template <typename... Ts>
struct overloaded : Ts... {
  using Ts::operator()...;
};

template <typename... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

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

} // namespace

void to_json(nlohmann::json& j, const RegistrationRequest& request) {
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

void from_json(const nlohmann::json& j, RegistrationRequest& request) {
  request.protocol_version = required<std::string>(j, "protocol_version");
  request.request_id = required<std::string>(j, "request_id");
  request.agent_id = required<std::string>(j, "agent_id");
  request.occurred_at = required<std::string>(j, "occurred_at");
  request.agent_version = required<std::string>(j, "agent_version");
  request.hostname = required<std::string>(j, "hostname");
  request.os = required<std::string>(j, "os");
  request.arch = required<std::string>(j, "arch");
}

void to_json(nlohmann::json& j, const HeartbeatRequest& request) {
  j = {
      {"protocol_version", request.protocol_version},
      {"request_id", request.request_id},
      {"agent_id", request.agent_id},
      {"occurred_at", request.occurred_at},
      {"agent_version", request.agent_version},
  };
}

void from_json(const nlohmann::json& j, HeartbeatRequest& request) {
  request.protocol_version = required<std::string>(j, "protocol_version");
  request.request_id = required<std::string>(j, "request_id");
  request.agent_id = required<std::string>(j, "agent_id");
  request.occurred_at = required<std::string>(j, "occurred_at");
  request.agent_version = required<std::string>(j, "agent_version");
}

void to_json(nlohmann::json& j, const AssetSnapshotRequest& request) {
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

void from_json(const nlohmann::json& j, AssetSnapshotRequest& request) {
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

void to_json(nlohmann::json& j, const StatusResponse& response) {
  j = {
      {"protocol_version", response.protocol_version},
      {"request_id", response.request_id},
      {"agent_id", response.agent_id},
      {"occurred_at", response.occurred_at},
      {"status", response.status},
      {"server_time", response.server_time},
  };
}

void from_json(const nlohmann::json& j, StatusResponse& response) {
  response.protocol_version = required<std::string>(j, "protocol_version");
  response.request_id = required<std::string>(j, "request_id");
  response.agent_id = required<std::string>(j, "agent_id");
  response.occurred_at = required<std::string>(j, "occurred_at");
  response.status = required<std::string>(j, "status");
  response.server_time = required<std::string>(j, "server_time");
}

void to_json(nlohmann::json& j, const ErrorResponse& response) {
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

void from_json(const nlohmann::json& j, ErrorResponse& response) {
  response.protocol_version = required<std::string>(j, "protocol_version");
  response.request_id = required<std::string>(j, "request_id");
  assign_optional(j, "agent_id", response.agent_id);
  response.occurred_at = required<std::string>(j, "occurred_at");
  response.error_code =
      parse_error_code(required<std::string>(j, "error_code"));
  response.message = required<std::string>(j, "message");
  response.retryable = required<bool>(j, "retryable");
}

void to_json(nlohmann::json& j, const AuditEvent& event) {
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

void from_json(const nlohmann::json& j, AuditEvent& event) {
  event.audit_id = required<std::string>(j, "audit_id");
  event.occurred_at = required<std::string>(j, "occurred_at");
  assign_optional(j, "agent_id", event.agent_id);
  event.request_id = required<std::string>(j, "request_id");
  event.event_type =
      parse_audit_event_type(required<std::string>(j, "event_type"));
  event.result = required<std::string>(j, "result");
  event.payload_json = required<std::string>(j, "payload_json");
}

void to_json(nlohmann::json& j, const CollectBasicInventoryInput&) {
  j = json::object();
}

void from_json(const nlohmann::json& j, CollectBasicInventoryInput& input) {
  if (!j.is_object()) {
    throw std::invalid_argument("collect_basic_inventory input must be object");
  }
  input = CollectBasicInventoryInput{};
}

void to_json(nlohmann::json& j, const CollectBasicInventoryResult& result) {
  j = {
      {"hostname", result.hostname},
      {"os", result.os},
      {"arch", result.arch},
      {"agent_version", result.agent_version},
  };
}

void from_json(const nlohmann::json& j, CollectBasicInventoryResult& result) {
  result.hostname = required<std::string>(j, "hostname");
  result.os = required<std::string>(j, "os");
  result.arch = required<std::string>(j, "arch");
  result.agent_version = required<std::string>(j, "agent_version");
}

void to_json(nlohmann::json& j, const Task& task) {
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

void from_json(const nlohmann::json& j, Task& task) {
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

void to_json(nlohmann::json& j, const TaskCreateRequest& request) {
  j = {
      {"protocol_version", request.protocol_version},
      {"request_id", request.request_id},
      {"occurred_at", request.occurred_at},
      {"task", request.task},
  };
}

void from_json(const nlohmann::json& j, TaskCreateRequest& request) {
  request.protocol_version = required<std::string>(j, "protocol_version");
  request.request_id = required<std::string>(j, "request_id");
  request.occurred_at = required<std::string>(j, "occurred_at");
  request.task = required<Task>(j, "task");
}

void to_json(nlohmann::json& j, const TaskPollResponse& response) {
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

void from_json(const nlohmann::json& j, TaskPollResponse& response) {
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

void to_json(nlohmann::json& j, const TaskError& error) {
  j = {
      {"error_code", ToString(error.error_code)},
      {"message", error.message},
      {"retryable", error.retryable},
  };
}

void from_json(const nlohmann::json& j, TaskError& error) {
  error.error_code = parse_error_code(required<std::string>(j, "error_code"));
  error.message = required<std::string>(j, "message");
  error.retryable = required<bool>(j, "retryable");
}

void to_json(nlohmann::json& j, const TaskRunningRequest& request) {
  j = {
      {"protocol_version", request.protocol_version},
      {"request_id", request.request_id},
      {"task_id", request.task_id},
      {"agent_id", request.agent_id},
      {"task_type", ToString(request.task_type)},
      {"occurred_at", request.occurred_at},
  };
}

void from_json(const nlohmann::json& j, TaskRunningRequest& request) {
  request.protocol_version = required<std::string>(j, "protocol_version");
  request.request_id = required<std::string>(j, "request_id");
  request.task_id = required<std::string>(j, "task_id");
  request.agent_id = required<std::string>(j, "agent_id");
  request.task_type = parse_task_type(required<std::string>(j, "task_type"));
  request.occurred_at = required<std::string>(j, "occurred_at");
}

void to_json(nlohmann::json& j, const TaskResultRequest& request) {
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

void from_json(const nlohmann::json& j, TaskResultRequest& request) {
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

} // namespace zfleet::protocol
