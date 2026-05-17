#pragma once

#include "zfleet/protocol/message.h"

#include <nlohmann/json.hpp>

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

} // namespace zfleet::protocol
