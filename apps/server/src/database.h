#pragma once

#include "zfleet/protocol/message.h"

#include <filesystem>
#include <string>

namespace zfleet::server {

class ServerDatabase {
 public:
  struct StoredTask {
    zfleet::protocol::Task task;
    zfleet::protocol::TaskState state;
    std::optional<std::string> assigned_at;
    std::optional<std::string> completed_at;
  };

  explicit ServerDatabase(std::filesystem::path database_path);

  void Initialize();
  int schema_version() const;
  const std::filesystem::path& database_path() const noexcept;
  bool AgentExists(const std::string& agent_id) const;
  void UpsertAgent(const zfleet::protocol::RegistrationRequest& request);
  void RecordHeartbeat(const zfleet::protocol::HeartbeatRequest& request,
                       const std::string& payload_json);
  void RecordAssetSnapshot(
      const zfleet::protocol::AssetSnapshotRequest& request,
      const std::string& payload_json);
  void RecordAuditEvent(const zfleet::protocol::AuditEvent& event);
  void EnqueueTask(const zfleet::protocol::Task& task);
  void MarkTaskRunning(const zfleet::protocol::TaskRunningRequest& request);
  std::optional<zfleet::protocol::Task> ClaimNextTaskForAgent(
      const std::string& agent_id,
      const std::string& assigned_at);
  std::optional<StoredTask> FindTaskById(const std::string& task_id) const;
  void RecordTaskResult(const zfleet::protocol::TaskResultRequest& request,
                        const std::optional<std::string>& result_json,
                        const std::optional<std::string>& error_json);

 private:
  std::filesystem::path database_path_;
};

} // namespace zfleet::server
