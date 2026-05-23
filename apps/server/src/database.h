#pragma once

#include "zfleet/protocol/message.h"

#include <chrono>
#include <condition_variable>
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
      const zfleet::protocol::AgentRegistration& request) = 0;
  virtual void RecordHeartbeat(
      const zfleet::protocol::AgentHeartbeat& request,
      const std::string& payload_json) = 0;
  virtual void RecordAssetSnapshot(
      const zfleet::protocol::AssetSnapshot& request,
      const std::string& payload_json) = 0;
  virtual void RecordAuditEvent(const zfleet::protocol::AuditEvent& event) = 0;
  virtual void EnqueueTask(const zfleet::protocol::Task& task) = 0;
  virtual std::uint64_t TaskQueueVersion() const = 0;
  virtual std::uint64_t WaitForTaskQueueChange(
      std::uint64_t last_seen_version,
      std::chrono::milliseconds timeout) const = 0;
  virtual void MarkTaskRunning(
      const zfleet::protocol::TaskRunning& request) = 0;
  virtual std::optional<zfleet::protocol::Task> ClaimNextTaskForAgent(
      const std::string& agent_id,
      const std::string& assigned_at) = 0;
  virtual std::optional<StoredTask> FindTaskById(
      const std::string& task_id) const = 0;
  virtual void RecordTaskResult(
      const zfleet::protocol::TaskResult& request,
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
  void UpsertAgent(const zfleet::protocol::AgentRegistration& request) override;
  void RecordHeartbeat(const zfleet::protocol::AgentHeartbeat& request,
                       const std::string& payload_json) override;
  void RecordAssetSnapshot(
      const zfleet::protocol::AssetSnapshot& request,
      const std::string& payload_json) override;
  void RecordAuditEvent(const zfleet::protocol::AuditEvent& event) override;
  void EnqueueTask(const zfleet::protocol::Task& task) override;
  std::uint64_t TaskQueueVersion() const override;
  std::uint64_t WaitForTaskQueueChange(
      std::uint64_t last_seen_version,
      std::chrono::milliseconds timeout) const override;
  void MarkTaskRunning(
      const zfleet::protocol::TaskRunning& request) override;
  std::optional<zfleet::protocol::Task> ClaimNextTaskForAgent(
      const std::string& agent_id,
      const std::string& assigned_at) override;
  std::optional<StoredTask> FindTaskById(
      const std::string& task_id) const override;
  void RecordTaskResult(const zfleet::protocol::TaskResult& request,
                        const std::optional<std::string>& result_json,
                        const std::optional<std::string>& error_json) override;

 private:
  std::filesystem::path database_path_;
  mutable std::mutex write_mutex_;
  mutable std::mutex task_queue_mutex_;
  mutable std::condition_variable task_queue_cv_;
  std::uint64_t task_queue_version_ = 0;
};

} // namespace zfleet::server
