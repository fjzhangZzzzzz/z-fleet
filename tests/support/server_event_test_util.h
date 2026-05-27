#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include "zfleet/protocol/v1/agent_control.pb.h"

namespace zfleet::server {
class ServerDatabase;
}

namespace zfleet::test {

namespace proto = zfleet::protocol::v1;

std::string ReadAgentField(const std::filesystem::path& database_path,
                           const std::string& agent_id,
                           const std::string& column_name);
std::string ReadAuditField(const std::filesystem::path& database_path,
                           const std::string& request_id,
                           const std::string& column_name);
std::string ReadTaskField(const std::filesystem::path& database_path,
                          const std::string& task_id,
                          const std::string& column_name);
std::string ReadTaskResultField(const std::filesystem::path& database_path,
                                const std::string& task_id,
                                const std::string& column_name);
std::string ReadTaskResultBlob(const std::filesystem::path& database_path,
                               const std::string& task_id,
                               const std::string& column_name);

proto::AgentEvent RegisterEvent(std::string message_id, std::string agent_id,
                                std::string occurred_at,
                                std::string registration_token = {},
                                std::string agent_version = "0.1.0");
proto::AgentEvent HeartbeatEvent(std::string message_id, std::string agent_id,
                                 std::string occurred_at);
proto::AgentEvent AssetSnapshotEvent(std::string message_id,
                                     std::string agent_id,
                                     std::string occurred_at);
proto::AgentEvent TaskRunningEvent(std::string message_id, std::string agent_id,
                                   std::string task_id,
                                   std::string occurred_at);
proto::AgentEvent TaskSucceededEvent(std::string message_id,
                                     std::string agent_id,
                                     std::string task_id,
                                     std::string occurred_at);
proto::AgentEvent TaskFailedEvent(std::string message_id, std::string agent_id,
                                  std::string task_id,
                                  std::string occurred_at);

void SeedAgent(zfleet::server::ServerDatabase* database,
               const std::string& agent_id);
void SeedTask(zfleet::server::ServerDatabase* database,
              const std::string& task_id, const std::string& agent_id);

}  // namespace zfleet::test
