#pragma once

#include "zfleet/protocol/message.h"

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>

namespace zfleet::server {

struct StoredTask {
  zfleet::protocol::Task task;
  zfleet::protocol::TaskState state;
  std::optional<std::string> assigned_at;
  std::optional<std::string> completed_at;
};

class ServerStore {
 public:
  virtual ~ServerStore() = default;

  virtual bool AgentExists(const std::string& agent_id) const = 0;
  virtual void UpsertAgent(
      const zfleet::protocol::RegistrationRequest& request) = 0;
  virtual void RecordHeartbeat(
      const zfleet::protocol::HeartbeatRequest& request,
      const std::string& payload_json) = 0;
  virtual void RecordAssetSnapshot(
      const zfleet::protocol::AssetSnapshotRequest& request,
      const std::string& payload_json) = 0;
  virtual void RecordAuditEvent(const zfleet::protocol::AuditEvent& event) = 0;
  virtual void EnqueueTask(const zfleet::protocol::Task& task) = 0;
  virtual void MarkTaskRunning(
      const zfleet::protocol::TaskRunningRequest& request) = 0;
  virtual std::optional<zfleet::protocol::Task> ClaimNextTaskForAgent(
      const std::string& agent_id,
      const std::string& assigned_at) = 0;
  virtual std::optional<StoredTask> FindTaskById(
      const std::string& task_id) const = 0;
  virtual void RecordTaskResult(
      const zfleet::protocol::TaskResultRequest& request,
      const std::optional<std::string>& result_json,
      const std::optional<std::string>& error_json) = 0;
};

class ServerDatabase final : public ServerStore {
 public:
  explicit ServerDatabase(std::filesystem::path database_path);

  void Initialize();
  int schema_version() const;
  const std::filesystem::path& database_path() const noexcept;
  bool AgentExists(const std::string& agent_id) const override;
  void UpsertAgent(const zfleet::protocol::RegistrationRequest& request) override;
  void RecordHeartbeat(const zfleet::protocol::HeartbeatRequest& request,
                       const std::string& payload_json) override;
  void RecordAssetSnapshot(
      const zfleet::protocol::AssetSnapshotRequest& request,
      const std::string& payload_json) override;
  void RecordAuditEvent(const zfleet::protocol::AuditEvent& event) override;
  void EnqueueTask(const zfleet::protocol::Task& task) override;
  void MarkTaskRunning(
      const zfleet::protocol::TaskRunningRequest& request) override;
  std::optional<zfleet::protocol::Task> ClaimNextTaskForAgent(
      const std::string& agent_id,
      const std::string& assigned_at) override;
  std::optional<StoredTask> FindTaskById(
      const std::string& task_id) const override;
  void RecordTaskResult(const zfleet::protocol::TaskResultRequest& request,
                        const std::optional<std::string>& result_json,
                        const std::optional<std::string>& error_json) override;

 private:
  std::filesystem::path database_path_;
  mutable std::mutex write_mutex_;
};

} // namespace zfleet::server
