#pragma once

#include "zfleet/protocol/message.h"

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace zfleet::protocol {

enum class JsonCodecErrorCode {
  invalid_json,
  missing_required_field,
  invalid_field_type,
  invalid_field_value,
};

struct JsonCodecContext {
  std::optional<std::string> request_id;
  std::optional<std::string> agent_id;
};

struct JsonCodecError {
  JsonCodecErrorCode code;
  std::string message;
  JsonCodecContext context;
};

template <typename T>
using JsonDecodeResult = std::variant<T, JsonCodecError>;

using AuditPayloadValue = std::variant<std::string, bool, std::int64_t>;

struct AuditPayloadField {
  std::string_view key;
  AuditPayloadValue value;
};

std::string SerializeAgentRegistration(const AgentRegistration& request);
JsonDecodeResult<AgentRegistration> ParseAgentRegistration(
    std::string_view json_text);

std::string SerializeAgentHeartbeat(const AgentHeartbeat& request);
JsonDecodeResult<AgentHeartbeat> ParseAgentHeartbeat(
    std::string_view json_text);

std::string SerializeAssetSnapshot(
    const AssetSnapshot& request);
JsonDecodeResult<AssetSnapshot> ParseAssetSnapshot(
    std::string_view json_text);

std::string SerializeAuditEvent(const AuditEvent& event);
JsonDecodeResult<AuditEvent> ParseAuditEvent(std::string_view json_text);

std::string SerializeCollectBasicInventoryInput(
    const CollectBasicInventoryInput& input);
JsonDecodeResult<CollectBasicInventoryInput> ParseCollectBasicInventoryInput(
    std::string_view json_text);

std::string SerializeCollectBasicInventoryResult(
    const CollectBasicInventoryResult& result);
JsonDecodeResult<CollectBasicInventoryResult> ParseCollectBasicInventoryResult(
    std::string_view json_text);

std::string SerializeTask(const Task& task);
JsonDecodeResult<Task> ParseTask(std::string_view json_text);

std::string SerializeTaskCreation(const TaskCreation& request);
JsonDecodeResult<TaskCreation> ParseTaskCreation(
    std::string_view json_text);

std::string SerializeTaskError(const TaskError& error);
JsonDecodeResult<TaskError> ParseTaskError(std::string_view json_text);

std::string SerializeTaskRunning(const TaskRunning& request);
JsonDecodeResult<TaskRunning> ParseTaskRunning(
    std::string_view json_text);

std::string SerializeTaskResult(const TaskResult& request);
JsonDecodeResult<TaskResult> ParseTaskResult(
    std::string_view json_text);

std::string SerializeTaskInput(const TaskInput& input);
JsonDecodeResult<TaskInput> ParseTaskInput(TaskType type,
                                           std::string_view json_text);

std::string SerializeTaskResultData(const TaskResultData& result);
JsonDecodeResult<TaskResultData> ParseTaskResultData(TaskType type,
                                                     std::string_view json_text);

std::string SerializeAuditPayload(
    std::initializer_list<AuditPayloadField> fields);

} // namespace zfleet::protocol
