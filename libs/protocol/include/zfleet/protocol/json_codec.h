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

} // namespace zfleet::protocol
