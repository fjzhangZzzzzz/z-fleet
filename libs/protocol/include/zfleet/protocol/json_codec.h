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

std::string SerializeRegistrationRequest(const RegistrationRequest& request);
JsonDecodeResult<RegistrationRequest> ParseRegistrationRequest(
    std::string_view json_text);

std::string SerializeHeartbeatRequest(const HeartbeatRequest& request);
JsonDecodeResult<HeartbeatRequest> ParseHeartbeatRequest(
    std::string_view json_text);

std::string SerializeAssetSnapshotRequest(
    const AssetSnapshotRequest& request);
JsonDecodeResult<AssetSnapshotRequest> ParseAssetSnapshotRequest(
    std::string_view json_text);

std::string SerializeStatusResponse(const StatusResponse& response);
JsonDecodeResult<StatusResponse> ParseStatusResponse(std::string_view json_text);

std::string SerializeErrorResponse(const ErrorResponse& response);
JsonDecodeResult<ErrorResponse> ParseErrorResponse(std::string_view json_text);

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

std::string SerializeTaskCreateRequest(const TaskCreateRequest& request);
JsonDecodeResult<TaskCreateRequest> ParseTaskCreateRequest(
    std::string_view json_text);

std::string SerializeTaskError(const TaskError& error);
JsonDecodeResult<TaskError> ParseTaskError(std::string_view json_text);

std::string SerializeTaskRunningRequest(const TaskRunningRequest& request);
JsonDecodeResult<TaskRunningRequest> ParseTaskRunningRequest(
    std::string_view json_text);

std::string SerializeTaskResultRequest(const TaskResultRequest& request);
JsonDecodeResult<TaskResultRequest> ParseTaskResultRequest(
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
