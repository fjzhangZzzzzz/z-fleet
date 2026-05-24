#pragma once

#include "zfleet/protocol/message.h"
#include "zfleet/protocol/v1/agent_control.pb.h"

#include <boost/asio/any_io_executor.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace SQLite {
class Database;
}

namespace zfleet::server {

struct AgentSummary {
  std::string agent_id;
  std::string first_seen_at;
  std::string last_seen_at;
  std::string last_online_at;
  std::optional<std::string> last_offline_at;
  std::string platform;
  std::string agent_version;
  std::string status;
};

struct AssetSnapshotSummary {
  std::int64_t snapshot_id = 0;
  std::string agent_id;
  std::string occurred_at;
  std::string hostname;
  std::string os;
  std::optional<std::string> os_version;
  std::string arch;
  std::string agent_version;
};

struct AgentPackageRecord {
  std::string package_id;
  std::string component;
  std::string version;
  std::string platform;
  std::string arch;
  std::string filename;
  std::filesystem::path storage_path;
  std::uint64_t size_bytes = 0;
  std::string sha256;
  std::string manifest_json;
  std::string status;
  std::string uploaded_at;
  std::optional<std::string> validated_at;
  std::optional<std::string> published_at;
  std::optional<std::string> retired_at;
};

struct RegistrationTokenRecord {
  std::string token_id;
  std::string token_hash;
  std::string purpose;
  std::optional<std::string> channel;
  std::optional<std::string> platform;
  std::optional<std::string> arch;
  std::optional<int> max_uses;
  int use_count = 0;
  std::string status;
  std::string created_at;
  std::string expires_at;
  std::optional<std::string> revoked_at;
};

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
  virtual void MarkAgentOffline(const std::string& agent_id,
                                const std::string& disconnected_at) = 0;
  virtual void RecordAssetSnapshot(
      const zfleet::protocol::AssetSnapshot& request,
      const zfleet::protocol::v1::AgentEvent& event) = 0;
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
      const std::optional<std::string>& result_blob,
      const std::optional<std::string>& error_blob) = 0;
};

class AsyncServerStore {
 public:
  virtual ~AsyncServerStore() = default;

  virtual void AsyncAgentExists(
      std::string agent_id,
      boost::asio::any_io_executor completion_executor,
      std::function<void(std::exception_ptr, bool)> completion) = 0;
  virtual void AsyncUpsertAgent(
      zfleet::protocol::AgentRegistration request,
      boost::asio::any_io_executor completion_executor,
      std::function<void(std::exception_ptr)> completion) = 0;
  virtual void AsyncMarkAgentOffline(
      std::string agent_id,
      std::string disconnected_at,
      boost::asio::any_io_executor completion_executor,
      std::function<void(std::exception_ptr)> completion) = 0;
  virtual void AsyncRecordAssetSnapshot(
      zfleet::protocol::AssetSnapshot request,
      zfleet::protocol::v1::AgentEvent event,
      boost::asio::any_io_executor completion_executor,
      std::function<void(std::exception_ptr)> completion) = 0;
  virtual void AsyncRecordAuditEvent(
      zfleet::protocol::AuditEvent event,
      boost::asio::any_io_executor completion_executor,
      std::function<void(std::exception_ptr)> completion) = 0;
  virtual void AsyncEnqueueTask(
      zfleet::protocol::Task task,
      boost::asio::any_io_executor completion_executor,
      std::function<void(std::exception_ptr)> completion) = 0;
  virtual void AsyncMarkTaskRunning(
      zfleet::protocol::TaskRunning request,
      boost::asio::any_io_executor completion_executor,
      std::function<void(std::exception_ptr)> completion) = 0;
  virtual void AsyncClaimNextTaskForAgent(
      std::string agent_id,
      std::string assigned_at,
      boost::asio::any_io_executor completion_executor,
      std::function<void(std::exception_ptr,
                         std::optional<zfleet::protocol::Task>)>
          completion) = 0;
  virtual void AsyncFindTaskById(
      std::string task_id,
      boost::asio::any_io_executor completion_executor,
      std::function<void(std::exception_ptr, std::optional<StoredTask>)>
          completion) = 0;
  virtual void AsyncRecordTaskResult(
      zfleet::protocol::TaskResult request,
      std::optional<std::string> result_blob,
      std::optional<std::string> error_blob,
      boost::asio::any_io_executor completion_executor,
      std::function<void(std::exception_ptr)> completion) = 0;
};

class ServerDatabase final : public ServerStore, public AsyncServerStore {
 public:
  struct TaskQueueSubscription {
    std::uint64_t id = 0;
  };

  using TaskQueueObserver = std::function<void(std::uint64_t)>;

  explicit ServerDatabase(std::filesystem::path database_path);
  ~ServerDatabase() override;

  void Initialize();
  void Stop();
  int schema_version() const;
  const std::filesystem::path& database_path() const noexcept;
  bool AgentExists(const std::string& agent_id) const override;
  void UpsertAgent(const zfleet::protocol::AgentRegistration& request) override;
  void MarkAgentOffline(const std::string& agent_id,
                        const std::string& disconnected_at) override;
  void RecordAssetSnapshot(
      const zfleet::protocol::AssetSnapshot& request,
      const zfleet::protocol::v1::AgentEvent& event) override;
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
                        const std::optional<std::string>& result_blob,
                        const std::optional<std::string>& error_blob) override;
  std::vector<AgentSummary> ListAgents() const;
  std::optional<AgentSummary> FindAgent(const std::string& agent_id) const;
  std::vector<AssetSnapshotSummary> ListAssetSnapshots(
      const std::string& agent_id,
      int limit) const;
  std::optional<AssetSnapshotSummary> FindLatestAssetSnapshot(
      const std::string& agent_id) const;
  void UpsertAgentPackage(const AgentPackageRecord& package);
  std::vector<AgentPackageRecord> ListAgentPackages() const;
  std::optional<AgentPackageRecord> FindAgentPackage(
      const std::string& package_id) const;
  void PublishAgentPackage(const std::string& package_id,
                           const std::string& channel,
                           const std::string& platform,
                           const std::string& arch,
                           const std::optional<std::string>& published_by,
                           const std::string& published_at);
  std::optional<AgentPackageRecord> FindDefaultPublishedAgentPackage(
      const std::string& channel,
      const std::string& platform,
      const std::string& arch) const;
  void CreateRegistrationToken(const RegistrationTokenRecord& token);
  std::vector<RegistrationTokenRecord> ListRegistrationTokens() const;
  void AsyncAgentExists(
      std::string agent_id,
      boost::asio::any_io_executor completion_executor,
      std::function<void(std::exception_ptr, bool)> completion) override;
  void AsyncUpsertAgent(
      zfleet::protocol::AgentRegistration request,
      boost::asio::any_io_executor completion_executor,
      std::function<void(std::exception_ptr)> completion) override;
  void AsyncMarkAgentOffline(
      std::string agent_id,
      std::string disconnected_at,
      boost::asio::any_io_executor completion_executor,
      std::function<void(std::exception_ptr)> completion) override;
  void AsyncRecordAssetSnapshot(
      zfleet::protocol::AssetSnapshot request,
      zfleet::protocol::v1::AgentEvent event,
      boost::asio::any_io_executor completion_executor,
      std::function<void(std::exception_ptr)> completion) override;
  void AsyncRecordAuditEvent(
      zfleet::protocol::AuditEvent event,
      boost::asio::any_io_executor completion_executor,
      std::function<void(std::exception_ptr)> completion) override;
  void AsyncEnqueueTask(
      zfleet::protocol::Task task,
      boost::asio::any_io_executor completion_executor,
      std::function<void(std::exception_ptr)> completion) override;
  void AsyncMarkTaskRunning(
      zfleet::protocol::TaskRunning request,
      boost::asio::any_io_executor completion_executor,
      std::function<void(std::exception_ptr)> completion) override;
  void AsyncClaimNextTaskForAgent(
      std::string agent_id,
      std::string assigned_at,
      boost::asio::any_io_executor completion_executor,
      std::function<void(std::exception_ptr,
                         std::optional<zfleet::protocol::Task>)>
          completion) override;
  void AsyncFindTaskById(
      std::string task_id,
      boost::asio::any_io_executor completion_executor,
      std::function<void(std::exception_ptr, std::optional<StoredTask>)>
          completion) override;
  void AsyncRecordTaskResult(
      zfleet::protocol::TaskResult request,
      std::optional<std::string> result_blob,
      std::optional<std::string> error_blob,
      boost::asio::any_io_executor completion_executor,
      std::function<void(std::exception_ptr)> completion) override;

  TaskQueueSubscription SubscribeTaskQueueChanges(
      boost::asio::any_io_executor completion_executor,
      TaskQueueObserver observer);
  void UnsubscribeTaskQueueChanges(TaskQueueSubscription subscription);

 private:
  struct WriteTask {
    std::function<void(SQLite::Database&)> run;
    std::function<void(std::exception_ptr)> fail;
  };

  struct TaskQueueObserverEntry {
    boost::asio::any_io_executor completion_executor;
    TaskQueueObserver observer;
  };

  void RunWriteActor();
  void StopWriteActor();

  template <typename Func>
  auto SubmitWrite(Func&& func) -> std::invoke_result_t<Func, SQLite::Database&>;
  template <typename Func, typename Completion>
  bool SubmitWriteAsync(Func&& func,
                        boost::asio::any_io_executor completion_executor,
                        Completion&& completion);
  void NotifyTaskQueueChanged();

  std::filesystem::path database_path_;
  mutable std::mutex write_actor_mutex_;
  mutable std::condition_variable write_actor_cv_;
  std::deque<WriteTask> write_queue_;
  bool write_actor_stopping_ = false;
  std::thread write_actor_thread_;
  mutable std::mutex task_queue_mutex_;
  mutable std::condition_variable task_queue_cv_;
  std::uint64_t task_queue_version_ = 0;
  bool task_queue_closed_ = false;
  mutable std::mutex task_queue_observers_mutex_;
  std::uint64_t next_task_queue_subscription_id_ = 1;
  std::map<std::uint64_t, TaskQueueObserverEntry> task_queue_observers_;
};

} // namespace zfleet::server
