#include "database.h"

#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>
#include <nlohmann/json.hpp>

#include <boost/asio/post.hpp>

#include <chrono>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>
#include <utility>
#include <variant>

namespace zfleet::server {
namespace {

constexpr int kSchemaVersion = 9;

namespace proto = zfleet::protocol::v1;

std::optional<std::string> NullableOptionalColumn(SQLite::Statement& query,
                                                  int index) {
  if (query.isColumnNull(index)) {
    return std::nullopt;
  }
  return query.getColumn(index).getString();
}

std::string BlobColumn(SQLite::Statement& query, int index) {
  const auto column = query.getColumn(index);
  const auto size = column.getBytes();
  if (size == 0) {
    return {};
  }
  return std::string(static_cast<const char*>(column.getBlob()),
                     static_cast<std::size_t>(size));
}

void BindBlob(SQLite::Statement& statement, int index,
              const std::string& bytes) {
  statement.bind(index, bytes.data(), static_cast<int>(bytes.size()));
}

template <typename Message>
std::string SerializeProto(const Message& message) {
  std::string bytes;
  if (!message.SerializeToString(&bytes)) {
    throw std::runtime_error("failed to serialize protobuf message");
  }
  return bytes;
}

std::string SerializeTaskInputBlob(zfleet::protocol::TaskType task_type,
                                   const zfleet::protocol::TaskInput& input) {
  switch (task_type) {
    case zfleet::protocol::TaskType::collect_basic_inventory: {
      if (!std::holds_alternative<zfleet::protocol::CollectBasicInventoryInput>(
              input)) {
        throw std::runtime_error(
            "task input type mismatch for collect_basic_inventory");
      }
      proto::CollectBasicInventoryInput message;
      return SerializeProto(message);
    }
    case zfleet::protocol::TaskType::package_update: {
      if (!std::holds_alternative<zfleet::protocol::PackageUpdateInput>(
              input)) {
        throw std::runtime_error("task input type mismatch for package_update");
      }
      const auto& value = std::get<zfleet::protocol::PackageUpdateInput>(input);
      proto::PackageUpdateInput message;
      message.set_action(value.action);
      message.set_component(value.component);
      message.set_package_id(value.package_id);
      message.set_version(value.version);
      message.set_platform(value.platform);
      message.set_arch(value.arch);
      message.set_build_type(value.build_type);
      message.set_package_url(value.package_url);
      message.set_package_sha256(value.package_sha256);
      message.set_manifest_sha256(value.manifest_sha256);
      message.set_min_installer_version(value.min_installer_version);
      message.set_allow_downgrade(value.allow_downgrade);
      message.set_force(value.force);
      return SerializeProto(message);
    }
  }

  throw std::runtime_error("unsupported task type for stored task input");
}

zfleet::protocol::TaskInput ParseTaskInputBlob(
    zfleet::protocol::TaskType task_type,
    const std::string& input_blob) {
  switch (task_type) {
    case zfleet::protocol::TaskType::collect_basic_inventory: {
      proto::CollectBasicInventoryInput message;
      if (!message.ParseFromString(input_blob)) {
        throw std::runtime_error("failed to parse stored task input");
      }
      return zfleet::protocol::CollectBasicInventoryInput{};
    }
    case zfleet::protocol::TaskType::package_update: {
      proto::PackageUpdateInput message;
      if (!message.ParseFromString(input_blob)) {
        throw std::runtime_error("failed to parse stored package update input");
      }
      return zfleet::protocol::PackageUpdateInput{
          .action = message.action().empty() ? "apply" : message.action(),
          .component = message.component(),
          .package_id = message.package_id(),
          .version = message.version(),
          .platform = message.platform(),
          .arch = message.arch(),
          .build_type = message.build_type(),
          .package_url = message.package_url(),
          .package_sha256 = message.package_sha256(),
          .manifest_sha256 = message.manifest_sha256(),
          .min_installer_version = message.min_installer_version(),
          .allow_downgrade = message.allow_downgrade(),
          .force = message.force(),
      };
    }
  }

  throw std::runtime_error("unsupported stored task input type");
}

zfleet::protocol::Task ParseStoredTask(SQLite::Statement& query) {
  const auto task_type = *zfleet::protocol::TaskTypeFromString(
      query.getColumn(3).getString());
  return zfleet::protocol::Task{
      .protocol_version = query.getColumn(0).getString(),
      .task_id = query.getColumn(1).getString(),
      .agent_id = query.getColumn(2).getString(),
      .task_type = task_type,
      .capability_level = *zfleet::protocol::CapabilityLevelFromString(
          query.getColumn(4).getString()),
      .created_at = query.getColumn(5).getString(),
      .expires_at = query.getColumn(6).getString(),
      .input = ParseTaskInputBlob(task_type, BlobColumn(query, 7)),
  };
}

AgentSummary ParseAgentSummary(SQLite::Statement& query) {
  return AgentSummary{
      .agent_id = query.getColumn(0).getString(),
      .first_seen_at = query.getColumn(1).getString(),
      .last_seen_at = query.getColumn(2).getString(),
      .last_online_at = query.getColumn(3).getString(),
      .last_offline_at = NullableOptionalColumn(query, 4),
      .platform = query.getColumn(5).getString(),
      .agent_version = query.getColumn(6).getString(),
      .status = query.getColumn(7).getString(),
      .current_package_id = NullableOptionalColumn(query, 8),
      .desired_version = NullableOptionalColumn(query, 9),
      .desired_package_id = NullableOptionalColumn(query, 10),
      .desired_set_at = NullableOptionalColumn(query, 11),
      .desired_set_by = NullableOptionalColumn(query, 12),
      .upgrade_state = NullableOptionalColumn(query, 13),
      .last_upgrade_task_id = NullableOptionalColumn(query, 14),
      .last_upgrade_error = NullableOptionalColumn(query, 15),
      .last_upgrade_at = NullableOptionalColumn(query, 16),
  };
}

AssetSnapshotSummary ParseAssetSnapshotSummary(SQLite::Statement& query) {
  return AssetSnapshotSummary{
      .snapshot_id = query.getColumn(0).getInt64(),
      .agent_id = query.getColumn(1).getString(),
      .occurred_at = query.getColumn(2).getString(),
      .hostname = query.getColumn(3).getString(),
      .os = query.getColumn(4).getString(),
      .os_version = NullableOptionalColumn(query, 5),
      .arch = query.getColumn(6).getString(),
      .agent_version = query.getColumn(7).getString(),
      .applications = nlohmann::json::parse(query.getColumn(8).getString())
                          .get<std::vector<std::string>>(),
      .services = nlohmann::json::parse(query.getColumn(9).getString())
                      .get<std::vector<std::string>>(),
  };
}

AgentPackageRecord ParseAgentPackageRecord(SQLite::Statement& query) {
  return AgentPackageRecord{
      .package_id = query.getColumn(0).getString(),
      .component = query.getColumn(1).getString(),
      .version = query.getColumn(2).getString(),
      .platform = query.getColumn(3).getString(),
      .arch = query.getColumn(4).getString(),
      .build_type = query.getColumn(5).getString(),
      .filename = query.getColumn(6).getString(),
      .storage_path = query.getColumn(7).getString(),
      .size_bytes = static_cast<std::uint64_t>(query.getColumn(8).getInt64()),
      .sha256 = query.getColumn(9).getString(),
      .manifest_json = query.getColumn(10).getString(),
      .status = query.getColumn(11).getString(),
      .uploaded_at = query.getColumn(12).getString(),
      .validated_at = NullableOptionalColumn(query, 13),
      .published_at = NullableOptionalColumn(query, 14),
      .retired_at = NullableOptionalColumn(query, 15),
  };
}

RegistrationTokenRecord ParseRegistrationTokenRecord(SQLite::Statement& query) {
  std::optional<int> max_uses;
  if (!query.isColumnNull(6)) {
    max_uses = query.getColumn(6).getInt();
  }
  return RegistrationTokenRecord{
      .token_id = query.getColumn(0).getString(),
      .token_hash = query.getColumn(1).getString(),
      .purpose = query.getColumn(2).getString(),
      .channel = NullableOptionalColumn(query, 3),
      .platform = NullableOptionalColumn(query, 4),
      .arch = NullableOptionalColumn(query, 5),
      .max_uses = max_uses,
      .use_count = query.getColumn(7).getInt(),
      .status = query.getColumn(8).getString(),
      .created_at = query.getColumn(9).getString(),
      .expires_at = query.getColumn(10).getString(),
      .revoked_at = NullableOptionalColumn(query, 11),
  };
}

bool ColumnExists(SQLite::Database& db, const std::string& table,
                  const std::string& column) {
  SQLite::Statement query(db, "pragma table_info(" + table + ")");
  while (query.executeStep()) {
    if (query.getColumn(1).getString() == column) {
      return true;
    }
  }
  return false;
}

void ExecSchema(SQLite::Database& db) {
  db.exec(R"sql(
    create table if not exists agents (
      agent_id text primary key,
      first_seen_at text not null,
      last_seen_at text not null,
      last_online_at text not null,
      last_offline_at text,
      platform text not null,
      agent_version text not null,
      status text not null,
      current_package_id text,
      desired_version text,
      desired_package_id text,
      desired_set_at text,
      desired_set_by text,
      upgrade_state text,
      last_upgrade_task_id text,
      last_upgrade_error text,
      last_upgrade_at text
    )
  )sql");

  db.exec(R"sql(
    create table if not exists asset_snapshots (
      snapshot_id integer primary key autoincrement,
      agent_id text not null,
      occurred_at text not null,
      hostname text not null,
      os text not null,
      os_version text,
      arch text not null,
      agent_version text not null,
      applications_json text not null,
      services_json text not null,
      event_blob blob not null
    )
  )sql");

  db.exec(R"sql(
    create table if not exists audit_events (
      audit_id text primary key,
      occurred_at text not null,
      agent_id text,
      event_type text not null,
      request_id text,
      payload_json text not null
    )
  )sql");

  db.exec(R"sql(
    create table if not exists tasks (
      task_id text primary key,
      protocol_version text not null,
      agent_id text not null,
      task_type text not null,
      capability_level text not null,
      created_at text not null,
      expires_at text not null,
      input_blob blob not null,
      state text not null,
      prerequisite_task_id text,
      assigned_at text,
      completed_at text
    )
  )sql");

  db.exec(R"sql(
    create table if not exists task_results (
      task_id text primary key,
      protocol_version text not null,
      request_id text not null,
      agent_id text not null,
      task_type text not null,
      occurred_at text not null,
      status text not null,
      error_code text,
      error_retryable integer,
      result_blob blob,
      error_blob blob
    )
  )sql");

  db.exec(R"sql(
    create table if not exists agent_packages (
      package_id text primary key,
      component text not null,
      version text not null,
      platform text not null,
      arch text not null,
      build_type text not null,
      filename text not null,
      storage_path text not null,
      size_bytes integer not null,
      sha256 text not null,
      manifest_json text not null,
      status text not null,
      uploaded_at text not null,
      validated_at text,
      published_at text,
      retired_at text
    )
  )sql");

  db.exec(R"sql(
    create table if not exists package_publications (
      publication_id text primary key,
      package_id text not null,
      channel text not null,
      component text not null,
      platform text not null,
      arch text not null,
      build_type text not null,
      is_default integer not null,
      published_at text not null,
      published_by text,
      foreign key(package_id) references agent_packages(package_id)
    )
  )sql");

  db.exec(R"sql(
    create table if not exists registration_tokens (
      token_id text primary key,
      token_hash text not null,
      purpose text not null,
      channel text,
      platform text,
      arch text,
      max_uses integer,
      use_count integer not null,
      status text not null,
      created_at text not null,
      expires_at text not null,
      revoked_at text
    )
  )sql");

  for (const auto* column : {"current_package_id", "desired_version",
                             "desired_package_id", "desired_set_at",
                             "desired_set_by", "upgrade_state",
                             "last_upgrade_task_id", "last_upgrade_error",
                             "last_upgrade_at"}) {
    if (!ColumnExists(db, "agents", column)) {
      db.exec(std::string("alter table agents add column ") + column +
              " text");
    }
  }
  if (!ColumnExists(db, "agent_packages", "build_type")) {
    db.exec("alter table agent_packages add column build_type text not null "
            "default 'release'");
  }
  if (!ColumnExists(db, "package_publications", "component")) {
    db.exec("alter table package_publications add column component text not null "
            "default 'agent'");
  }
  if (!ColumnExists(db, "package_publications", "build_type")) {
    db.exec("alter table package_publications add column build_type text not null "
            "default 'release'");
  }
  if (!ColumnExists(db, "tasks", "prerequisite_task_id")) {
    db.exec("alter table tasks add column prerequisite_task_id text");
  }
  db.exec("drop index if exists package_publications_default_idx");
  db.exec(R"sql(
    create unique index if not exists package_publications_default_idx
      on package_publications(component, channel, platform, arch, build_type)
      where is_default = 1
  )sql");

  db.exec("PRAGMA user_version = " + std::to_string(kSchemaVersion));
}

constexpr int kBusyTimeoutMs = 5000;
constexpr int kBusyRetryAttempts = 5;
constexpr auto kBusyRetryDelay = std::chrono::milliseconds(25);

SQLite::Database OpenDatabase(const std::filesystem::path& database_path,
                              const int flags) {
  SQLite::Database db(database_path.string(), flags);
  db.exec("PRAGMA busy_timeout=" + std::to_string(kBusyTimeoutMs));
  return db;
}

bool IsRecoverableBusy(const SQLite::Exception& ex) {
  const auto error_code = ex.getErrorCode();
  return error_code == SQLITE_BUSY || error_code == SQLITE_LOCKED;
}

template <typename Func>
auto ExecuteWithBusyRetry(Func&& func) -> std::invoke_result_t<Func> {
  for (int attempt = 0;; ++attempt) {
    try {
      return func();
    } catch (const SQLite::Exception& ex) {
      if (!IsRecoverableBusy(ex) || attempt >= kBusyRetryAttempts) {
        throw;
      }
      std::this_thread::sleep_for(kBusyRetryDelay);
    }
  }
}

} // namespace

ServerDatabase::ServerDatabase(std::filesystem::path database_path)
    : database_path_(std::move(database_path)),
      write_actor_thread_(&ServerDatabase::RunWriteActor, this) {}

ServerDatabase::~ServerDatabase() {
  Stop();
}

void ServerDatabase::Stop() {
  StopWriteActor();
  {
    std::lock_guard lock(task_queue_mutex_);
    task_queue_closed_ = true;
  }
  task_queue_cv_.notify_all();
  {
    std::lock_guard lock(task_queue_observers_mutex_);
    task_queue_observers_.clear();
  }
}

void ServerDatabase::RunWriteActor() {
  std::optional<SQLite::Database> db;
  while (true) {
    WriteTask task;
    {
      std::unique_lock lock(write_actor_mutex_);
      write_actor_cv_.wait(lock, [&]() {
        return write_actor_stopping_ || !write_queue_.empty();
      });
      if (write_queue_.empty()) {
        if (write_actor_stopping_) {
          return;
        }
        continue;
      }
      task = std::move(write_queue_.front());
      write_queue_.pop_front();
    }

    try {
      if (!db.has_value()) {
        db.emplace(OpenDatabase(database_path_,
                                SQLite::OPEN_READWRITE |
                                    SQLite::OPEN_CREATE));
      }
      task.run(*db);
    } catch (...) {
      task.fail(std::current_exception());
    }
  }
}

void ServerDatabase::StopWriteActor() {
  {
    std::lock_guard lock(write_actor_mutex_);
    if (write_actor_stopping_) {
      return;
    }
    write_actor_stopping_ = true;
  }
  write_actor_cv_.notify_all();
  if (write_actor_thread_.joinable()) {
    write_actor_thread_.join();
  }
}

template <typename Func>
auto ServerDatabase::SubmitWrite(Func&& func)
    -> std::invoke_result_t<Func, SQLite::Database&> {
  using Result = std::invoke_result_t<Func, SQLite::Database&>;

  auto promise = std::make_shared<std::promise<Result>>();
  auto result = promise->get_future();
  {
    std::lock_guard lock(write_actor_mutex_);
    if (write_actor_stopping_) {
      throw std::runtime_error("server database write actor is stopped");
    }
    write_queue_.push_back(WriteTask{
        .run = [func = std::forward<Func>(func),
                promise](SQLite::Database& db) mutable {
          try {
            if constexpr (std::is_void_v<Result>) {
              ExecuteWithBusyRetry([&]() {
                func(db);
              });
              promise->set_value();
            } else {
              promise->set_value(ExecuteWithBusyRetry([&]() {
                return func(db);
              }));
            }
          } catch (...) {
            promise->set_exception(std::current_exception());
          }
        },
        .fail = [promise](std::exception_ptr exception) {
          promise->set_exception(exception);
        },
    });
  }
  write_actor_cv_.notify_one();

  if constexpr (std::is_void_v<Result>) {
    result.get();
  } else {
    return result.get();
  }
}

template <typename Func, typename Completion>
bool ServerDatabase::SubmitWriteAsync(
    Func&& func,
    boost::asio::any_io_executor completion_executor,
    Completion&& completion) {
  using Result = std::invoke_result_t<Func, SQLite::Database&>;
  auto shared_completion =
      std::make_shared<std::decay_t<Completion>>(std::forward<Completion>(
          completion));
  {
    std::lock_guard lock(write_actor_mutex_);
    if (write_actor_stopping_) {
      const auto exception = std::make_exception_ptr(
          std::runtime_error("server database write actor is stopped"));
      if constexpr (std::is_void_v<Result>) {
        boost::asio::post(
            completion_executor,
            [completion = shared_completion, exception]() mutable {
              (*completion)(exception);
            });
      } else {
        boost::asio::post(
            completion_executor,
            [completion = shared_completion, exception]() mutable {
              (*completion)(exception, Result{});
            });
      }
      return true;
    }
    write_queue_.push_back(WriteTask{
        .run = [func = std::forward<Func>(func),
                completion_executor,
                completion = shared_completion](
                   SQLite::Database& db) mutable {
          try {
            if constexpr (std::is_void_v<Result>) {
              ExecuteWithBusyRetry([&]() {
                func(db);
              });
              boost::asio::post(completion_executor,
                                [completion]() mutable {
                                  (*completion)(nullptr);
                                });
            } else {
              auto value = ExecuteWithBusyRetry([&]() {
                return func(db);
              });
              boost::asio::post(
                  completion_executor,
                  [completion, value = std::move(value)]() mutable {
                    (*completion)(nullptr, std::move(value));
                  });
            }
          } catch (...) {
            const auto exception = std::current_exception();
            if constexpr (std::is_void_v<Result>) {
              boost::asio::post(
                  completion_executor,
                  [completion, exception]() mutable {
                    (*completion)(exception);
                  });
            } else {
              boost::asio::post(
                  completion_executor,
                  [completion, exception]() mutable {
                    (*completion)(exception, Result{});
                  });
            }
          }
        },
        .fail = [completion_executor, completion = shared_completion](
                    std::exception_ptr exception) mutable {
          if constexpr (std::is_void_v<Result>) {
            boost::asio::post(
                completion_executor,
                [completion, exception]() mutable {
                  (*completion)(exception);
                });
          } else {
            boost::asio::post(
                completion_executor,
                [completion, exception]() mutable {
                  (*completion)(exception, Result{});
                });
          }
        },
    });
  }
  write_actor_cv_.notify_one();
  return true;
}

void ServerDatabase::NotifyTaskQueueChanged() {
  std::vector<TaskQueueObserverEntry> observers;
  std::uint64_t version = 0;
  {
    std::lock_guard lock(task_queue_mutex_);
    version = ++task_queue_version_;
  }
  task_queue_cv_.notify_all();
  {
    std::lock_guard lock(task_queue_observers_mutex_);
    observers.reserve(task_queue_observers_.size());
    for (const auto& [_, observer] : task_queue_observers_) {
      observers.push_back(observer);
    }
  }

  for (auto& observer : observers) {
    boost::asio::post(
        observer.completion_executor,
        [callback = std::move(observer.observer), version]() mutable {
          callback(version);
        });
  }
}

void ServerDatabase::Initialize() {
  if (database_path_.empty()) {
    throw std::runtime_error("database path must not be empty");
  }

  const auto parent_path = database_path_.parent_path();
  if (!parent_path.empty()) {
    std::filesystem::create_directories(parent_path);
  }

  SubmitWrite([](SQLite::Database& db) {
    db.exec("PRAGMA journal_mode=WAL");
    ExecSchema(db);
  });
}

int ServerDatabase::schema_version() const {
  return ExecuteWithBusyRetry([&]() {
    SQLite::Database db = OpenDatabase(database_path_, SQLite::OPEN_READONLY);
    SQLite::Statement query(db, "PRAGMA user_version");
    if (!query.executeStep()) {
      throw std::runtime_error("failed to read schema version");
    }
    return query.getColumn(0).getInt();
  });
}

const std::filesystem::path& ServerDatabase::database_path() const noexcept {
  return database_path_;
}

bool ServerDatabase::AgentExists(const std::string& agent_id) const {
  return ExecuteWithBusyRetry([&]() {
    SQLite::Database db = OpenDatabase(database_path_, SQLite::OPEN_READONLY);
    SQLite::Statement query(db, "select 1 from agents where agent_id = ?");
    query.bind(1, agent_id);
    return query.executeStep();
  });
}

bool ServerDatabase::ConsumeRegistrationToken(const std::string& token_hash,
                                              const std::string& platform,
                                              const std::string& arch,
                                              const std::string& used_at) {
  return SubmitWrite([&](SQLite::Database& db) {
    SQLite::Statement statement(
        db,
        R"sql(
        update registration_tokens
        set use_count = use_count + 1,
            status = case when max_uses is not null and use_count + 1 >= max_uses
                          then 'consumed' else status end
        where token_hash = ?
          and purpose = 'agent_register'
          and status = 'active'
          and expires_at >= ?
          and (platform is null or platform = ?)
          and (arch is null or arch = ?)
          and (max_uses is null or use_count < max_uses)
      )sql");
    statement.bind(1, token_hash);
    statement.bind(2, used_at);
    statement.bind(3, platform);
    statement.bind(4, arch);
    statement.exec();
    return db.getChanges() == 1;
  });
}

void ServerDatabase::UpsertAgent(
    const zfleet::protocol::AgentRegistration& request) {
  SubmitWrite([&](SQLite::Database& db) {
    SQLite::Statement statement(
        db,
        R"sql(
        insert into agents (
          agent_id,
          first_seen_at,
          last_seen_at,
          last_online_at,
          last_offline_at,
          platform,
          agent_version,
          status,
          current_package_id,
          desired_version,
          desired_package_id,
          desired_set_at,
          desired_set_by,
          upgrade_state,
          last_upgrade_task_id,
          last_upgrade_error,
          last_upgrade_at
        ) values (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        on conflict(agent_id) do update set
          last_seen_at = case
            when agents.status != 'online' then excluded.last_seen_at
            else agents.last_seen_at
          end,
          last_online_at = case
            when agents.status != 'online' then excluded.last_online_at
            else agents.last_online_at
          end,
          last_offline_at = case
            when agents.status != 'online' then null
            else agents.last_offline_at
          end,
          platform = excluded.platform,
          agent_version = excluded.agent_version,
          status = excluded.status,
          current_package_id = case
            when agents.upgrade_state = 'waiting_reconnect'
              and agents.desired_version = excluded.agent_version
              then agents.desired_package_id
            when agents.upgrade_state = 'waiting_reconnect'
              and agents.desired_package_id is null
              then null
            else agents.current_package_id
          end,
          upgrade_state = case
            when agents.upgrade_state = 'waiting_reconnect'
              and agents.desired_version = excluded.agent_version
              then 'succeeded'
            when agents.upgrade_state = 'waiting_reconnect'
              and agents.desired_package_id is null
              then 'succeeded'
            when agents.upgrade_state = 'waiting_reconnect'
              and agents.desired_version is not null
              and agents.desired_version != excluded.agent_version
              then 'failed'
            else agents.upgrade_state
          end,
          last_upgrade_error = case
            when agents.upgrade_state = 'waiting_reconnect'
              and agents.desired_version = excluded.agent_version
              then null
            when agents.upgrade_state = 'waiting_reconnect'
              and agents.desired_package_id is null
              then null
            when agents.upgrade_state = 'waiting_reconnect'
              and agents.desired_version is not null
              and agents.desired_version != excluded.agent_version
              then 'agent_reported_unexpected_version'
            else agents.last_upgrade_error
          end,
          last_upgrade_at = case
            when agents.upgrade_state = 'waiting_reconnect'
              and agents.desired_version = excluded.agent_version
              then excluded.last_online_at
            when agents.upgrade_state = 'waiting_reconnect'
              and agents.desired_package_id is null
              then excluded.last_online_at
            when agents.upgrade_state = 'waiting_reconnect'
              and agents.desired_version is not null
              and agents.desired_version != excluded.agent_version
              then excluded.last_online_at
            else agents.last_upgrade_at
          end
      )sql");
    statement.bind(1, request.agent_id);
    statement.bind(2, request.occurred_at);
    statement.bind(3, request.occurred_at);
    statement.bind(4, request.occurred_at);
    statement.bind(5);
    statement.bind(6, request.os);
    statement.bind(7, request.agent_version);
    statement.bind(8, "online");
    for (int index = 9; index <= 17; ++index) {
      statement.bind(index);
    }
    statement.exec();
  });
}

void ServerDatabase::MarkAgentOffline(const std::string& agent_id,
                                      const std::string& disconnected_at) {
  SubmitWrite([&](SQLite::Database& db) {
    SQLite::Statement statement(
        db,
        "update agents set last_seen_at = ?, last_offline_at = ?, status = ? "
        "where agent_id = ? and status != ?");
    statement.bind(1, disconnected_at);
    statement.bind(2, disconnected_at);
    statement.bind(3, "offline");
    statement.bind(4, agent_id);
    statement.bind(5, "offline");
    statement.exec();
  });
}

void ServerDatabase::RecordAssetSnapshot(
    const zfleet::protocol::AssetSnapshot& request,
    const proto::AgentEvent& event) {
  const auto event_blob = SerializeProto(event);
  SubmitWrite([&](SQLite::Database& db) {
    SQLite::Statement statement(
        db,
        R"sql(
        insert into asset_snapshots (
          agent_id,
          occurred_at,
          hostname,
          os,
          os_version,
          arch,
          agent_version,
          applications_json,
          services_json,
          event_blob
        ) values (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
      )sql");
    statement.bind(1, request.agent_id);
    statement.bind(2, request.occurred_at);
    statement.bind(3, request.hostname);
    statement.bind(4, request.os);
    if (request.os_version.has_value()) {
      statement.bind(5, *request.os_version);
    } else {
      statement.bind(5);
    }
    statement.bind(6, request.arch);
    statement.bind(7, request.agent_version);
    statement.bind(8, nlohmann::json(request.applications).dump());
    statement.bind(9, nlohmann::json(request.services).dump());
    BindBlob(statement, 10, event_blob);
    statement.exec();
  });
}

void ServerDatabase::RecordAuditEvent(const zfleet::protocol::AuditEvent& event) {
  SubmitWrite([&](SQLite::Database& db) {
    SQLite::Statement statement(
        db,
        R"sql(
        insert into audit_events (
          audit_id,
          occurred_at,
          agent_id,
          event_type,
          request_id,
          payload_json
        ) values (?, ?, ?, ?, ?, ?)
      )sql");
    statement.bind(1, event.audit_id);
    statement.bind(2, event.occurred_at);
    if (event.agent_id.has_value()) {
      statement.bind(3, *event.agent_id);
    } else {
      statement.bind(3);
    }
    statement.bind(4,
                   std::string(zfleet::protocol::ToString(event.event_type)));
    statement.bind(5, event.request_id);
    statement.bind(6, event.payload_json);
    statement.exec();
  });
}

void ServerDatabase::EnqueueTask(const zfleet::protocol::Task& task) {
  SubmitWrite([&](SQLite::Database& db) {
    SQLite::Statement statement(
        db,
        R"sql(
          insert into tasks (
            task_id,
            protocol_version,
            agent_id,
            task_type,
            capability_level,
            created_at,
            expires_at,
            input_blob,
            state,
            assigned_at,
            completed_at
          ) values (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        )sql");
    statement.bind(1, task.task_id);
    statement.bind(2, task.protocol_version);
    statement.bind(3, task.agent_id);
    statement.bind(4, std::string(zfleet::protocol::ToString(task.task_type)));
    statement.bind(
        5, std::string(zfleet::protocol::ToString(task.capability_level)));
    statement.bind(6, task.created_at);
    statement.bind(7, task.expires_at);
    BindBlob(statement, 8, SerializeTaskInputBlob(task.task_type, task.input));
    statement.bind(9, std::string(zfleet::protocol::ToString(
                          zfleet::protocol::TaskState::queued)));
    statement.bind(10);
    statement.bind(11);
    statement.exec();
  });

  NotifyTaskQueueChanged();
}

std::uint64_t ServerDatabase::TaskQueueVersion() const {
  std::lock_guard lock(task_queue_mutex_);
  return task_queue_version_;
}

std::uint64_t ServerDatabase::WaitForTaskQueueChange(
    std::uint64_t last_seen_version,
    std::chrono::milliseconds timeout) const {
  std::unique_lock lock(task_queue_mutex_);
  task_queue_cv_.wait_for(lock, timeout, [&]() {
    return task_queue_version_ != last_seen_version || task_queue_closed_;
  });
  return task_queue_version_;
}

void ServerDatabase::MarkTaskRunning(
    const zfleet::protocol::TaskRunning& request) {
  SubmitWrite([&](SQLite::Database& db) {
    SQLite::Statement update(
        db,
        "update tasks set state = ?, assigned_at = coalesce(assigned_at, ?) "
        "where task_id = ? and state = ?");
    update.bind(
        1,
        std::string(zfleet::protocol::ToString(
            zfleet::protocol::TaskState::running)));
    update.bind(2, request.occurred_at);
    update.bind(3, request.task_id);
    update.bind(
        4,
        std::string(zfleet::protocol::ToString(
            zfleet::protocol::TaskState::assigned)));
    update.exec();
    if (sqlite3_changes(db.getHandle()) != 1) {
      throw std::runtime_error("task running state transition failed");
    }
  });
}

std::optional<zfleet::protocol::Task> ServerDatabase::ClaimNextTaskForAgent(
    const std::string& agent_id,
    const std::string& assigned_at) {
  const auto task = SubmitWrite([&](SQLite::Database& db) {
    SQLite::Transaction transaction(db);
    SQLite::Statement select(
        db,
        R"sql(
        select
          protocol_version,
          task_id,
          agent_id,
          task_type,
          capability_level,
          created_at,
          expires_at,
          input_blob
        from tasks
        where agent_id = ?
          and state = ?
          and expires_at > ?
          and (
            prerequisite_task_id is null or exists (
              select 1 from tasks prerequisite
              where prerequisite.task_id = tasks.prerequisite_task_id
                and prerequisite.state = 'succeeded'
            )
          )
        order by created_at asc
        limit 1
      )sql");
    select.bind(1, agent_id);
    select.bind(2, std::string(zfleet::protocol::ToString(
                       zfleet::protocol::TaskState::queued)));
    select.bind(3, assigned_at);
    if (!select.executeStep()) {
      return std::optional<zfleet::protocol::Task>{};
    }

    const auto task = ParseStoredTask(select);
    SQLite::Statement update(
        db,
        "update tasks set state = ?, assigned_at = ? "
        "where task_id = ? and state = ?");
    update.bind(
        1,
        std::string(zfleet::protocol::ToString(
            zfleet::protocol::TaskState::assigned)));
    update.bind(2, assigned_at);
    update.bind(3, task.task_id);
    update.bind(
        4,
        std::string(zfleet::protocol::ToString(
            zfleet::protocol::TaskState::queued)));
    update.exec();
    if (sqlite3_changes(db.getHandle()) != 1) {
      return std::optional<zfleet::protocol::Task>{};
    }
    transaction.commit();
    return std::optional<zfleet::protocol::Task>{task};
  });
  if (task.has_value()) {
    NotifyTaskQueueChanged();
  }
  return task;
}

std::optional<StoredTask> ServerDatabase::FindTaskById(
    const std::string& task_id) const {
  return ExecuteWithBusyRetry([&]() {
    SQLite::Database db = OpenDatabase(database_path_, SQLite::OPEN_READONLY);
    SQLite::Statement query(
        db,
        R"sql(
        select
          protocol_version,
          task_id,
          agent_id,
          task_type,
          capability_level,
          created_at,
          expires_at,
          input_blob,
          state,
          assigned_at,
          completed_at
        from tasks
        where task_id = ?
      )sql");
    query.bind(1, task_id);
    if (!query.executeStep()) {
      return std::optional<StoredTask>{};
    }

    return std::optional<StoredTask>{StoredTask{
        .task = ParseStoredTask(query),
        .state = *zfleet::protocol::TaskStateFromString(
            query.getColumn(8).getString()),
        .assigned_at = NullableOptionalColumn(query, 9),
        .completed_at = NullableOptionalColumn(query, 10),
    }};
  });
}

void ServerDatabase::AsyncAgentExists(
    std::string agent_id,
    boost::asio::any_io_executor completion_executor,
    std::function<void(std::exception_ptr, bool)> completion) {
  if (!SubmitWriteAsync(
          [agent_id = std::move(agent_id)](SQLite::Database& db) {
            SQLite::Statement query(
                db, "select 1 from agents where agent_id = ?");
            query.bind(1, agent_id);
            return query.executeStep();
          },
          completion_executor, std::move(completion))) {
    boost::asio::post(completion_executor, [completion = std::move(completion)] {
      completion(std::make_exception_ptr(
                     std::runtime_error("server database write actor is stopped")),
                 false);
    });
  }
}

void ServerDatabase::AsyncUpsertAgent(
    zfleet::protocol::AgentRegistration request,
    boost::asio::any_io_executor completion_executor,
    std::function<void(std::exception_ptr)> completion) {
  if (!SubmitWriteAsync(
          [request = std::move(request)](SQLite::Database& db) {
            SQLite::Statement statement(
                db,
                R"sql(
        insert into agents (
          agent_id,
          first_seen_at,
          last_seen_at,
          last_online_at,
          last_offline_at,
          platform,
          agent_version,
          status
        ) values (?, ?, ?, ?, ?, ?, ?, ?)
        on conflict(agent_id) do update set
          last_seen_at = case
            when agents.status != 'online' then excluded.last_seen_at
            else agents.last_seen_at
          end,
          last_online_at = case
            when agents.status != 'online' then excluded.last_online_at
            else agents.last_online_at
          end,
          last_offline_at = case
            when agents.status != 'online' then null
            else agents.last_offline_at
          end,
          platform = excluded.platform,
          agent_version = excluded.agent_version,
          status = excluded.status
      )sql");
            statement.bind(1, request.agent_id);
            statement.bind(2, request.occurred_at);
            statement.bind(3, request.occurred_at);
            statement.bind(4, request.occurred_at);
            statement.bind(5);
            statement.bind(6, request.os);
            statement.bind(7, request.agent_version);
            statement.bind(8, "online");
            statement.exec();
          },
          completion_executor, std::move(completion))) {
    boost::asio::post(completion_executor, [completion = std::move(completion)] {
      completion(std::make_exception_ptr(
          std::runtime_error("server database write actor is stopped")));
    });
  }
}

void ServerDatabase::AsyncMarkAgentOffline(
    std::string agent_id,
    std::string disconnected_at,
    boost::asio::any_io_executor completion_executor,
    std::function<void(std::exception_ptr)> completion) {
  if (!SubmitWriteAsync(
          [agent_id = std::move(agent_id),
           disconnected_at = std::move(disconnected_at)](SQLite::Database& db) {
            SQLite::Statement statement(
                db,
                "update agents set last_seen_at = ?, last_offline_at = ?, "
                "status = ? where agent_id = ? and status != ?");
            statement.bind(1, disconnected_at);
            statement.bind(2, disconnected_at);
            statement.bind(3, "offline");
            statement.bind(4, agent_id);
            statement.bind(5, "offline");
            statement.exec();
          },
          completion_executor, std::move(completion))) {
    boost::asio::post(completion_executor, [completion = std::move(completion)] {
      completion(std::make_exception_ptr(
          std::runtime_error("server database write actor is stopped")));
    });
  }
}

void ServerDatabase::AsyncRecordAssetSnapshot(
    zfleet::protocol::AssetSnapshot request,
    proto::AgentEvent event,
    boost::asio::any_io_executor completion_executor,
    std::function<void(std::exception_ptr)> completion) {
  auto event_blob = SerializeProto(event);
  if (!SubmitWriteAsync(
          [request = std::move(request),
           event_blob = std::move(event_blob)](SQLite::Database& db) {
            SQLite::Statement statement(
                db,
                R"sql(
        insert into asset_snapshots (
          agent_id,
          occurred_at,
          hostname,
          os,
          os_version,
          arch,
          agent_version,
          applications_json,
          services_json,
          event_blob
        ) values (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
      )sql");
            statement.bind(1, request.agent_id);
            statement.bind(2, request.occurred_at);
            statement.bind(3, request.hostname);
            statement.bind(4, request.os);
            if (request.os_version.has_value()) {
              statement.bind(5, *request.os_version);
            } else {
              statement.bind(5);
            }
            statement.bind(6, request.arch);
            statement.bind(7, request.agent_version);
            statement.bind(8, nlohmann::json(request.applications).dump());
            statement.bind(9, nlohmann::json(request.services).dump());
            BindBlob(statement, 10, event_blob);
            statement.exec();
          },
          completion_executor, std::move(completion))) {
    boost::asio::post(completion_executor, [completion = std::move(completion)] {
      completion(std::make_exception_ptr(
          std::runtime_error("server database write actor is stopped")));
    });
  }
}

void ServerDatabase::AsyncRecordAuditEvent(
    zfleet::protocol::AuditEvent event,
    boost::asio::any_io_executor completion_executor,
    std::function<void(std::exception_ptr)> completion) {
  if (!SubmitWriteAsync(
          [event = std::move(event)](SQLite::Database& db) {
            SQLite::Statement statement(
                db,
                R"sql(
        insert into audit_events (
          audit_id,
          occurred_at,
          agent_id,
          event_type,
          request_id,
          payload_json
        ) values (?, ?, ?, ?, ?, ?)
      )sql");
            statement.bind(1, event.audit_id);
            statement.bind(2, event.occurred_at);
            if (event.agent_id.has_value()) {
              statement.bind(3, *event.agent_id);
            } else {
              statement.bind(3);
            }
            statement.bind(
                4, std::string(zfleet::protocol::ToString(event.event_type)));
            statement.bind(5, event.request_id);
            statement.bind(6, event.payload_json);
            statement.exec();
          },
          completion_executor, std::move(completion))) {
    boost::asio::post(completion_executor, [completion = std::move(completion)] {
      completion(std::make_exception_ptr(
          std::runtime_error("server database write actor is stopped")));
    });
  }
}

void ServerDatabase::AsyncEnqueueTask(
    zfleet::protocol::Task task,
    boost::asio::any_io_executor completion_executor,
    std::function<void(std::exception_ptr)> completion) {
  if (!SubmitWriteAsync(
          [this, task = std::move(task)](SQLite::Database& db) {
            SQLite::Statement statement(
                db,
                R"sql(
          insert into tasks (
            task_id,
            protocol_version,
            agent_id,
            task_type,
            capability_level,
            created_at,
            expires_at,
            input_blob,
            state,
            assigned_at,
            completed_at
          ) values (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        )sql");
            statement.bind(1, task.task_id);
            statement.bind(2, task.protocol_version);
            statement.bind(3, task.agent_id);
            statement.bind(
                4, std::string(zfleet::protocol::ToString(task.task_type)));
            statement.bind(5, std::string(
                                  zfleet::protocol::ToString(
                                      task.capability_level)));
            statement.bind(6, task.created_at);
            statement.bind(7, task.expires_at);
            BindBlob(statement, 8,
                     SerializeTaskInputBlob(task.task_type, task.input));
            statement.bind(
                9, std::string(zfleet::protocol::ToString(
                       zfleet::protocol::TaskState::queued)));
            statement.bind(10);
            statement.bind(11);
            statement.exec();
            NotifyTaskQueueChanged();
          },
          completion_executor, std::move(completion))) {
    boost::asio::post(completion_executor, [completion = std::move(completion)] {
      completion(std::make_exception_ptr(
          std::runtime_error("server database write actor is stopped")));
    });
  }
}

void ServerDatabase::AsyncMarkTaskRunning(
    zfleet::protocol::TaskRunning request,
    boost::asio::any_io_executor completion_executor,
    std::function<void(std::exception_ptr)> completion) {
  if (!SubmitWriteAsync(
          [request = std::move(request)](SQLite::Database& db) {
            SQLite::Statement update(
                db,
                "update tasks set state = ?, "
                "assigned_at = coalesce(assigned_at, ?) "
                "where task_id = ? and state = ?");
            update.bind(
                1,
                std::string(zfleet::protocol::ToString(
                    zfleet::protocol::TaskState::running)));
            update.bind(2, request.occurred_at);
            update.bind(3, request.task_id);
            update.bind(
                4,
                std::string(zfleet::protocol::ToString(
                    zfleet::protocol::TaskState::assigned)));
            update.exec();
            if (sqlite3_changes(db.getHandle()) != 1) {
              throw std::runtime_error("task running state transition failed");
            }
          },
          completion_executor, std::move(completion))) {
    boost::asio::post(completion_executor, [completion = std::move(completion)] {
      completion(std::make_exception_ptr(
          std::runtime_error("server database write actor is stopped")));
    });
  }
}

void ServerDatabase::AsyncClaimNextTaskForAgent(
    std::string agent_id,
    std::string assigned_at,
    boost::asio::any_io_executor completion_executor,
    std::function<void(std::exception_ptr,
                       std::optional<zfleet::protocol::Task>)> completion) {
  if (!SubmitWriteAsync(
          [this, agent_id = std::move(agent_id),
           assigned_at = std::move(assigned_at)](SQLite::Database& db) {
            SQLite::Transaction transaction(db);
            SQLite::Statement select(
                db,
                R"sql(
        select
          protocol_version,
          task_id,
          agent_id,
          task_type,
          capability_level,
          created_at,
          expires_at,
          input_blob
        from tasks
        where agent_id = ?
          and state = ?
          and expires_at > ?
          and (
            prerequisite_task_id is null or exists (
              select 1 from tasks prerequisite
              where prerequisite.task_id = tasks.prerequisite_task_id
                and prerequisite.state = 'succeeded'
            )
          )
        order by created_at asc
        limit 1
      )sql");
            select.bind(1, agent_id);
            select.bind(2, std::string(zfleet::protocol::ToString(
                               zfleet::protocol::TaskState::queued)));
            select.bind(3, assigned_at);
            if (!select.executeStep()) {
              return std::optional<zfleet::protocol::Task>{};
            }

            const auto task = ParseStoredTask(select);
            SQLite::Statement update(
                db,
                "update tasks set state = ?, assigned_at = ? "
                "where task_id = ? and state = ?");
            update.bind(
                1,
                std::string(zfleet::protocol::ToString(
                    zfleet::protocol::TaskState::assigned)));
            update.bind(2, assigned_at);
            update.bind(3, task.task_id);
            update.bind(
                4,
                std::string(zfleet::protocol::ToString(
                    zfleet::protocol::TaskState::queued)));
            update.exec();
            if (sqlite3_changes(db.getHandle()) != 1) {
              return std::optional<zfleet::protocol::Task>{};
            }
            transaction.commit();
            NotifyTaskQueueChanged();
            return std::optional<zfleet::protocol::Task>{task};
          },
          completion_executor, std::move(completion))) {
    boost::asio::post(completion_executor, [completion = std::move(completion)] {
      completion(std::make_exception_ptr(
                     std::runtime_error("server database write actor is stopped")),
                 std::nullopt);
    });
  }
}

ServerDatabase::TaskQueueSubscription ServerDatabase::SubscribeTaskQueueChanges(
    boost::asio::any_io_executor completion_executor,
    TaskQueueObserver observer) {
  if (!observer) {
    throw std::invalid_argument("task queue observer must not be empty");
  }

  std::lock_guard lock(task_queue_observers_mutex_);
  const auto id = next_task_queue_subscription_id_++;
  task_queue_observers_.insert_or_assign(
      id, TaskQueueObserverEntry{
              .completion_executor = std::move(completion_executor),
              .observer = std::move(observer),
          });
  return TaskQueueSubscription{id};
}

void ServerDatabase::UnsubscribeTaskQueueChanges(
    TaskQueueSubscription subscription) {
  if (subscription.id == 0) {
    return;
  }

  std::lock_guard lock(task_queue_observers_mutex_);
  task_queue_observers_.erase(subscription.id);
}

void ServerDatabase::AsyncFindTaskById(
    std::string task_id,
    boost::asio::any_io_executor completion_executor,
    std::function<void(std::exception_ptr, std::optional<StoredTask>)>
        completion) {
  if (!SubmitWriteAsync(
          [task_id = std::move(task_id)](SQLite::Database& db) {
            SQLite::Statement query(
                db,
                R"sql(
        select
          protocol_version,
          task_id,
          agent_id,
          task_type,
          capability_level,
          created_at,
          expires_at,
          input_blob,
          state,
          assigned_at,
          completed_at
        from tasks
        where task_id = ?
      )sql");
            query.bind(1, task_id);
            if (!query.executeStep()) {
              return std::optional<StoredTask>{};
            }

            return std::optional<StoredTask>{StoredTask{
                .task = ParseStoredTask(query),
                .state = *zfleet::protocol::TaskStateFromString(
                    query.getColumn(8).getString()),
                .assigned_at = NullableOptionalColumn(query, 9),
                .completed_at = NullableOptionalColumn(query, 10),
            }};
          },
          completion_executor, std::move(completion))) {
    boost::asio::post(completion_executor, [completion = std::move(completion)] {
      completion(std::make_exception_ptr(
                     std::runtime_error("server database write actor is stopped")),
                 std::nullopt);
    });
  }
}

void ServerDatabase::RecordTaskResult(
    const zfleet::protocol::TaskResult& request,
    const std::optional<std::string>& result_blob,
    const std::optional<std::string>& error_blob) {
  SubmitWrite([&](SQLite::Database& db) {
    SQLite::Transaction transaction(db);

    SQLite::Statement insert(
        db,
        R"sql(
        insert into task_results (
          task_id,
          protocol_version,
          request_id,
          agent_id,
          task_type,
          occurred_at,
          status,
          error_code,
          error_retryable,
          result_blob,
          error_blob
        ) values (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
      )sql");
    insert.bind(1, request.task_id);
    insert.bind(2, request.protocol_version);
    insert.bind(3, request.request_id);
    insert.bind(4, request.agent_id);
    insert.bind(5, std::string(zfleet::protocol::ToString(request.task_type)));
    insert.bind(6, request.occurred_at);
    insert.bind(7, std::string(zfleet::protocol::ToString(request.status)));
    if (request.error.has_value()) {
      insert.bind(8, std::string(zfleet::protocol::ToString(
                         request.error->error_code)));
      insert.bind(9, request.error->retryable ? 1 : 0);
    } else {
      insert.bind(8);
      insert.bind(9);
    }
    if (result_blob.has_value()) {
      BindBlob(insert, 10, *result_blob);
    } else {
      insert.bind(10);
    }
    if (error_blob.has_value()) {
      BindBlob(insert, 11, *error_blob);
    } else {
      insert.bind(11);
    }
    insert.exec();

    const auto terminal_state =
        request.status == zfleet::protocol::TaskExecutionStatus::succeeded
            ? zfleet::protocol::TaskState::succeeded
            : request.status == zfleet::protocol::TaskExecutionStatus::failed
                  ? zfleet::protocol::TaskState::failed
                  : zfleet::protocol::TaskState::expired;
    SQLite::Statement update(
        db,
        "update tasks set state = ?, completed_at = ? where task_id = ? "
        "and state not in (?, ?, ?)");
    update.bind(1, std::string(zfleet::protocol::ToString(terminal_state)));
    update.bind(2, request.occurred_at);
    update.bind(3, request.task_id);
    update.bind(
        4,
        std::string(zfleet::protocol::ToString(
            zfleet::protocol::TaskState::succeeded)));
    update.bind(
        5,
        std::string(zfleet::protocol::ToString(
            zfleet::protocol::TaskState::failed)));
    update.bind(
        6,
        std::string(zfleet::protocol::ToString(
            zfleet::protocol::TaskState::expired)));
    update.exec();
    if (sqlite3_changes(db.getHandle()) != 1) {
      throw std::runtime_error("task result state transition failed");
    }

    if (request.task_type == zfleet::protocol::TaskType::package_update) {
      const auto next_state =
          request.status == zfleet::protocol::TaskExecutionStatus::succeeded
              ? "waiting_reconnect"
              : "failed";
      SQLite::Statement update_agent(
          db,
          "update agents set upgrade_state = ?, last_upgrade_error = ?, "
          "last_upgrade_at = ? where agent_id = ? "
          "and last_upgrade_task_id = ?");
      update_agent.bind(1, next_state);
      if (request.error.has_value()) {
        update_agent.bind(
            2, std::string(zfleet::protocol::ToString(
                   request.error->error_code)));
      } else {
        update_agent.bind(2);
      }
      update_agent.bind(3, request.occurred_at);
      update_agent.bind(4, request.agent_id);
      update_agent.bind(5, request.task_id);
      update_agent.exec();
      if (request.status != zfleet::protocol::TaskExecutionStatus::succeeded) {
        SQLite::Statement fail_dependent(
            db,
            "update agents set upgrade_state = 'failed', "
            "last_upgrade_error = ?, last_upgrade_at = ? "
            "where agent_id = ? and last_upgrade_task_id in "
            "(select task_id from tasks where prerequisite_task_id = ?)");
        if (request.error.has_value()) {
          fail_dependent.bind(
              1, std::string(zfleet::protocol::ToString(
                     request.error->error_code)));
        } else {
          fail_dependent.bind(1, "task_execution_failed");
        }
        fail_dependent.bind(2, request.occurred_at);
        fail_dependent.bind(3, request.agent_id);
        fail_dependent.bind(4, request.task_id);
        fail_dependent.exec();
      }
    }

    transaction.commit();
  });
  NotifyTaskQueueChanged();
}

void ServerDatabase::ScheduleAgentUpgrade(
    const std::string& agent_id,
    const std::string& desired_version,
    const std::string& desired_package_id,
    const std::optional<std::string>& set_by,
    const std::string& set_at,
    const zfleet::protocol::Task& task,
    const std::optional<zfleet::protocol::Task>& prerequisite_task) {
  SubmitWrite([&](SQLite::Database& db) {
    SQLite::Transaction transaction(db);
    SQLite::Statement update(
        db,
        R"sql(
        update agents set
          desired_version = ?,
          desired_package_id = ?,
          desired_set_at = ?,
          desired_set_by = ?,
          upgrade_state = 'queued',
          last_upgrade_task_id = ?,
          last_upgrade_error = null
        where agent_id = ?
      )sql");
    update.bind(1, desired_version);
    update.bind(2, desired_package_id);
    update.bind(3, set_at);
    if (set_by.has_value()) {
      update.bind(4, *set_by);
    } else {
      update.bind(4);
    }
    update.bind(5, task.task_id);
    update.bind(6, agent_id);
    update.exec();
    if (sqlite3_changes(db.getHandle()) != 1) {
      throw std::runtime_error("agent not found for upgrade");
    }

    const auto insert_task =
        [&](const zfleet::protocol::Task& value,
            const std::optional<std::string>& prerequisite_task_id) {
          SQLite::Statement insert(
              db,
              R"sql(
        insert into tasks (
          task_id, protocol_version, agent_id, task_type, capability_level,
          created_at, expires_at, input_blob, state, prerequisite_task_id,
          assigned_at, completed_at
        ) values (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
      )sql");
          insert.bind(1, value.task_id);
          insert.bind(2, value.protocol_version);
          insert.bind(3, value.agent_id);
          insert.bind(4,
                      std::string(zfleet::protocol::ToString(value.task_type)));
          insert.bind(5, std::string(zfleet::protocol::ToString(
                             value.capability_level)));
          insert.bind(6, value.created_at);
          insert.bind(7, value.expires_at);
          BindBlob(insert, 8,
                   SerializeTaskInputBlob(value.task_type, value.input));
          insert.bind(9, "queued");
          if (prerequisite_task_id.has_value()) {
            insert.bind(10, *prerequisite_task_id);
          } else {
            insert.bind(10);
          }
          insert.bind(11);
          insert.bind(12);
          insert.exec();
        };
    if (prerequisite_task.has_value()) {
      insert_task(*prerequisite_task, std::nullopt);
      insert_task(task, prerequisite_task->task_id);
    } else {
      insert_task(task, std::nullopt);
    }
    transaction.commit();
  });
  NotifyTaskQueueChanged();
}

void ServerDatabase::ScheduleAgentRollback(
    const std::string& agent_id,
    const std::optional<std::string>& set_by,
    const std::string& set_at,
    const zfleet::protocol::Task& task) {
  ScheduleAgentUpgrade(agent_id, "", "", set_by, set_at, task);
  SubmitWrite([&](SQLite::Database& db) {
    SQLite::Statement update(
        db,
        "update agents set desired_version = null, desired_package_id = null "
        "where agent_id = ? and last_upgrade_task_id = ?");
    update.bind(1, agent_id);
    update.bind(2, task.task_id);
    update.exec();
  });
}

std::vector<std::string> ServerDatabase::ExpireWaitingReconnect(
    const std::string& now) {
  return SubmitWrite([&](SQLite::Database& db) {
    std::vector<std::string> expired;
    SQLite::Statement select(
        db,
        "select agent_id from agents where upgrade_state = "
        "'waiting_reconnect' and last_upgrade_at is not null "
        "and datetime(last_upgrade_at) <= datetime(?, '-10 minutes')");
    select.bind(1, now);
    while (select.executeStep()) {
      expired.push_back(select.getColumn(0).getString());
    }
    SQLite::Statement update(
        db,
        "update agents set upgrade_state = 'failed', "
        "last_upgrade_error = 'waiting_reconnect_timeout' "
        "where upgrade_state = 'waiting_reconnect' "
        "and last_upgrade_at is not null "
        "and datetime(last_upgrade_at) <= datetime(?, '-10 minutes')");
    update.bind(1, now);
    update.exec();
    return expired;
  });
}

std::vector<AgentSummary> ServerDatabase::ListAgents() const {
  return ExecuteWithBusyRetry([&]() {
    SQLite::Database db = OpenDatabase(database_path_, SQLite::OPEN_READONLY);
    SQLite::Statement query(
        db,
        R"sql(
        select
          agent_id,
          first_seen_at,
          last_seen_at,
          last_online_at,
          last_offline_at,
          platform,
          agent_version,
          status,
          current_package_id,
          desired_version,
          desired_package_id,
          desired_set_at,
          desired_set_by,
          upgrade_state,
          last_upgrade_task_id,
          last_upgrade_error,
          last_upgrade_at
        from agents
        order by status desc, last_seen_at desc, agent_id asc
      )sql");
    std::vector<AgentSummary> agents;
    while (query.executeStep()) {
      agents.push_back(ParseAgentSummary(query));
    }
    return agents;
  });
}

std::optional<AgentSummary> ServerDatabase::FindAgent(
    const std::string& agent_id) const {
  return ExecuteWithBusyRetry([&]() {
    SQLite::Database db = OpenDatabase(database_path_, SQLite::OPEN_READONLY);
    SQLite::Statement query(
        db,
        R"sql(
        select
          agent_id,
          first_seen_at,
          last_seen_at,
          last_online_at,
          last_offline_at,
          platform,
          agent_version,
          status,
          current_package_id,
          desired_version,
          desired_package_id,
          desired_set_at,
          desired_set_by,
          upgrade_state,
          last_upgrade_task_id,
          last_upgrade_error,
          last_upgrade_at
        from agents
        where agent_id = ?
      )sql");
    query.bind(1, agent_id);
    if (!query.executeStep()) {
      return std::optional<AgentSummary>{};
    }
    return std::optional<AgentSummary>{ParseAgentSummary(query)};
  });
}

std::vector<AssetSnapshotSummary> ServerDatabase::ListAssetSnapshots(
    const std::string& agent_id,
    int limit) const {
  return ExecuteWithBusyRetry([&]() {
    SQLite::Database db = OpenDatabase(database_path_, SQLite::OPEN_READONLY);
    SQLite::Statement query(
        db,
        R"sql(
        select
          snapshot_id,
          agent_id,
          occurred_at,
          hostname,
          os,
          os_version,
          arch,
          agent_version,
          applications_json,
          services_json
        from asset_snapshots
        where agent_id = ?
        order by snapshot_id desc
        limit ?
      )sql");
    query.bind(1, agent_id);
    query.bind(2, limit <= 0 ? 100 : limit);
    std::vector<AssetSnapshotSummary> snapshots;
    while (query.executeStep()) {
      snapshots.push_back(ParseAssetSnapshotSummary(query));
    }
    return snapshots;
  });
}

std::optional<AssetSnapshotSummary> ServerDatabase::FindLatestAssetSnapshot(
    const std::string& agent_id) const {
  const auto snapshots = ListAssetSnapshots(agent_id, 1);
  if (snapshots.empty()) {
    return std::nullopt;
  }
  return snapshots.front();
}

void ServerDatabase::UpsertAgentPackage(const AgentPackageRecord& package) {
  SubmitWrite([&](SQLite::Database& db) {
    SQLite::Statement statement(
        db,
        R"sql(
        insert into agent_packages (
          package_id,
          component,
          version,
          platform,
          arch,
          build_type,
          filename,
          storage_path,
          size_bytes,
          sha256,
          manifest_json,
          status,
          uploaded_at,
          validated_at,
          published_at,
          retired_at
        ) values (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        on conflict(package_id) do update set
          component = excluded.component,
          version = excluded.version,
          platform = excluded.platform,
          arch = excluded.arch,
          build_type = excluded.build_type,
          filename = excluded.filename,
          storage_path = excluded.storage_path,
          size_bytes = excluded.size_bytes,
          sha256 = excluded.sha256,
          manifest_json = excluded.manifest_json,
          status = excluded.status,
          uploaded_at = excluded.uploaded_at,
          validated_at = excluded.validated_at,
          published_at = excluded.published_at,
          retired_at = excluded.retired_at
      )sql");
    statement.bind(1, package.package_id);
    statement.bind(2, package.component);
    statement.bind(3, package.version);
    statement.bind(4, package.platform);
    statement.bind(5, package.arch);
    statement.bind(6, package.build_type);
    statement.bind(7, package.filename);
    statement.bind(8, package.storage_path.string());
    statement.bind(9, static_cast<std::int64_t>(package.size_bytes));
    statement.bind(10, package.sha256);
    statement.bind(11, package.manifest_json);
    statement.bind(12, package.status);
    statement.bind(13, package.uploaded_at);
    if (package.validated_at.has_value()) {
      statement.bind(14, *package.validated_at);
    } else {
      statement.bind(14);
    }
    if (package.published_at.has_value()) {
      statement.bind(15, *package.published_at);
    } else {
      statement.bind(15);
    }
    if (package.retired_at.has_value()) {
      statement.bind(16, *package.retired_at);
    } else {
      statement.bind(16);
    }
    statement.exec();
  });
}

std::vector<AgentPackageRecord> ServerDatabase::ListAgentPackages() const {
  return ExecuteWithBusyRetry([&]() {
    SQLite::Database db = OpenDatabase(database_path_, SQLite::OPEN_READONLY);
    SQLite::Statement query(
        db,
        R"sql(
        select
          package_id,
          component,
          version,
          platform,
          arch,
          build_type,
          filename,
          storage_path,
          size_bytes,
          sha256,
          manifest_json,
          status,
          uploaded_at,
          validated_at,
          published_at,
          retired_at
        from agent_packages
        order by uploaded_at desc, package_id asc
      )sql");
    std::vector<AgentPackageRecord> packages;
    while (query.executeStep()) {
      packages.push_back(ParseAgentPackageRecord(query));
    }
    return packages;
  });
}

std::optional<AgentPackageRecord> ServerDatabase::FindAgentPackage(
    const std::string& package_id) const {
  return ExecuteWithBusyRetry([&]() {
    SQLite::Database db = OpenDatabase(database_path_, SQLite::OPEN_READONLY);
    SQLite::Statement query(
        db,
        R"sql(
        select
          package_id,
          component,
          version,
          platform,
          arch,
          build_type,
          filename,
          storage_path,
          size_bytes,
          sha256,
          manifest_json,
          status,
          uploaded_at,
          validated_at,
          published_at,
          retired_at
        from agent_packages
        where package_id = ?
      )sql");
    query.bind(1, package_id);
    if (!query.executeStep()) {
      return std::optional<AgentPackageRecord>{};
    }
    return std::optional<AgentPackageRecord>{ParseAgentPackageRecord(query)};
  });
}

void ServerDatabase::PublishAgentPackage(
    const std::string& package_id,
    const std::string& component,
    const std::string& channel,
    const std::string& platform,
    const std::string& arch,
    const std::string& build_type,
    const std::optional<std::string>& published_by,
    const std::string& published_at) {
  SubmitWrite([&](SQLite::Database& db) {
    SQLite::Transaction transaction(db);
    SQLite::Statement clear(
        db,
        "update package_publications set is_default = 0 "
        "where component = ? and channel = ? and platform = ? and arch = ? "
        "and build_type = ? and is_default = 1");
    clear.bind(1, component);
    clear.bind(2, channel);
    clear.bind(3, platform);
    clear.bind(4, arch);
    clear.bind(5, build_type);
    clear.exec();

    const auto publication_id =
        package_id + ":" + channel + ":" + platform + ":" + arch + ":" +
        build_type;
    SQLite::Statement publish(
        db,
        R"sql(
        insert into package_publications (
          publication_id,
          package_id,
          channel,
          component,
          platform,
          arch,
          build_type,
          is_default,
          published_at,
          published_by
        ) values (?, ?, ?, ?, ?, ?, ?, 1, ?, ?)
        on conflict(publication_id) do update set
          package_id = excluded.package_id,
          channel = excluded.channel,
          component = excluded.component,
          platform = excluded.platform,
          arch = excluded.arch,
          build_type = excluded.build_type,
          is_default = excluded.is_default,
          published_at = excluded.published_at,
          published_by = excluded.published_by
      )sql");
    publish.bind(1, publication_id);
    publish.bind(2, package_id);
    publish.bind(3, channel);
    publish.bind(4, component);
    publish.bind(5, platform);
    publish.bind(6, arch);
    publish.bind(7, build_type);
    publish.bind(8, published_at);
    if (published_by.has_value()) {
      publish.bind(9, *published_by);
    } else {
      publish.bind(9);
    }
    publish.exec();

    SQLite::Statement update_package(
        db,
        "update agent_packages set status = ?, published_at = ? "
        "where package_id = ?");
    update_package.bind(1, "published");
    update_package.bind(2, published_at);
    update_package.bind(3, package_id);
    update_package.exec();
    if (sqlite3_changes(db.getHandle()) != 1) {
      throw std::runtime_error("agent package not found for publish");
    }
    transaction.commit();
  });
}

void ServerDatabase::RetireAgentPackage(const std::string& package_id,
                                        const std::string& retired_at) {
  SubmitWrite([&](SQLite::Database& db) {
    SQLite::Transaction transaction(db);
    SQLite::Statement update_package(
        db,
        "update agent_packages set status = 'retired', retired_at = ? "
        "where package_id = ?");
    update_package.bind(1, retired_at);
    update_package.bind(2, package_id);
    update_package.exec();
    if (sqlite3_changes(db.getHandle()) != 1) {
      throw std::runtime_error("agent package not found for retire");
    }
    SQLite::Statement disable_publications(
        db, "update package_publications set is_default = 0 where package_id = ?");
    disable_publications.bind(1, package_id);
    disable_publications.exec();
    transaction.commit();
  });
}

std::optional<AgentPackageRecord>
ServerDatabase::FindDefaultPublishedAgentPackage(
    const std::string& component,
    const std::string& channel,
    const std::string& platform,
    const std::string& arch,
    const std::string& build_type) const {
  return ExecuteWithBusyRetry([&]() {
    SQLite::Database db = OpenDatabase(database_path_, SQLite::OPEN_READONLY);
    SQLite::Statement query(
        db,
        R"sql(
        select
          p.package_id,
          p.component,
          p.version,
          p.platform,
          p.arch,
          p.build_type,
          p.filename,
          p.storage_path,
          p.size_bytes,
          p.sha256,
          p.manifest_json,
          p.status,
          p.uploaded_at,
          p.validated_at,
          p.published_at,
          p.retired_at
        from agent_packages p
        join package_publications pub on pub.package_id = p.package_id
        where pub.component = ?
          and pub.channel = ?
          and pub.platform = ?
          and pub.arch = ?
          and pub.build_type = ?
          and pub.is_default = 1
          and p.status = 'published'
        order by pub.published_at desc
        limit 1
      )sql");
    query.bind(1, component);
    query.bind(2, channel);
    query.bind(3, platform);
    query.bind(4, arch);
    query.bind(5, build_type);
    if (!query.executeStep()) {
      return std::optional<AgentPackageRecord>{};
    }
    return std::optional<AgentPackageRecord>{ParseAgentPackageRecord(query)};
  });
}

void ServerDatabase::CreateRegistrationToken(
    const RegistrationTokenRecord& token) {
  SubmitWrite([&](SQLite::Database& db) {
    SQLite::Statement statement(
        db,
        R"sql(
        insert into registration_tokens (
          token_id,
          token_hash,
          purpose,
          channel,
          platform,
          arch,
          max_uses,
          use_count,
          status,
          created_at,
          expires_at,
          revoked_at
        ) values (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
      )sql");
    statement.bind(1, token.token_id);
    statement.bind(2, token.token_hash);
    statement.bind(3, token.purpose);
    if (token.channel.has_value()) {
      statement.bind(4, *token.channel);
    } else {
      statement.bind(4);
    }
    if (token.platform.has_value()) {
      statement.bind(5, *token.platform);
    } else {
      statement.bind(5);
    }
    if (token.arch.has_value()) {
      statement.bind(6, *token.arch);
    } else {
      statement.bind(6);
    }
    if (token.max_uses.has_value()) {
      statement.bind(7, *token.max_uses);
    } else {
      statement.bind(7);
    }
    statement.bind(8, token.use_count);
    statement.bind(9, token.status);
    statement.bind(10, token.created_at);
    statement.bind(11, token.expires_at);
    if (token.revoked_at.has_value()) {
      statement.bind(12, *token.revoked_at);
    } else {
      statement.bind(12);
    }
    statement.exec();
  });
}

std::vector<RegistrationTokenRecord>
ServerDatabase::ListRegistrationTokens() const {
  return ExecuteWithBusyRetry([&]() {
    SQLite::Database db = OpenDatabase(database_path_, SQLite::OPEN_READONLY);
    SQLite::Statement query(
        db,
        R"sql(
        select
          token_id,
          token_hash,
          purpose,
          channel,
          platform,
          arch,
          max_uses,
          use_count,
          status,
          created_at,
          expires_at,
          revoked_at
        from registration_tokens
        order by created_at desc, token_id asc
      )sql");
    std::vector<RegistrationTokenRecord> tokens;
    while (query.executeStep()) {
      tokens.push_back(ParseRegistrationTokenRecord(query));
    }
    return tokens;
  });
}

void ServerDatabase::AsyncRecordTaskResult(
    zfleet::protocol::TaskResult request,
    std::optional<std::string> result_blob,
    std::optional<std::string> error_blob,
    boost::asio::any_io_executor completion_executor,
    std::function<void(std::exception_ptr)> completion) {
  if (!SubmitWriteAsync(
          [request = std::move(request), result_blob = std::move(result_blob),
           error_blob = std::move(error_blob)](SQLite::Database& db) {
            SQLite::Transaction transaction(db);

            SQLite::Statement insert(
                db,
                R"sql(
        insert into task_results (
          task_id,
          protocol_version,
          request_id,
          agent_id,
          task_type,
          occurred_at,
          status,
          error_code,
          error_retryable,
          result_blob,
          error_blob
        ) values (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
      )sql");
            insert.bind(1, request.task_id);
            insert.bind(2, request.protocol_version);
            insert.bind(3, request.request_id);
            insert.bind(4, request.agent_id);
            insert.bind(
                5, std::string(zfleet::protocol::ToString(request.task_type)));
            insert.bind(6, request.occurred_at);
            insert.bind(7,
                        std::string(zfleet::protocol::ToString(request.status)));
            if (request.error.has_value()) {
              insert.bind(
                  8, std::string(zfleet::protocol::ToString(
                         request.error->error_code)));
              insert.bind(9, request.error->retryable ? 1 : 0);
            } else {
              insert.bind(8);
              insert.bind(9);
            }
            if (result_blob.has_value()) {
              BindBlob(insert, 10, *result_blob);
            } else {
              insert.bind(10);
            }
            if (error_blob.has_value()) {
              BindBlob(insert, 11, *error_blob);
            } else {
              insert.bind(11);
            }
            insert.exec();

            const auto terminal_state =
                request.status ==
                        zfleet::protocol::TaskExecutionStatus::succeeded
                    ? zfleet::protocol::TaskState::succeeded
                    : request.status ==
                              zfleet::protocol::TaskExecutionStatus::failed
                          ? zfleet::protocol::TaskState::failed
                          : zfleet::protocol::TaskState::expired;
            SQLite::Statement update(
                db,
                "update tasks set state = ?, completed_at = ? "
                "where task_id = ? and state not in (?, ?, ?)");
            update.bind(
                1, std::string(zfleet::protocol::ToString(terminal_state)));
            update.bind(2, request.occurred_at);
            update.bind(3, request.task_id);
            update.bind(
                4,
                std::string(zfleet::protocol::ToString(
                    zfleet::protocol::TaskState::succeeded)));
            update.bind(
                5,
                std::string(zfleet::protocol::ToString(
                    zfleet::protocol::TaskState::failed)));
            update.bind(
                6,
                std::string(zfleet::protocol::ToString(
                    zfleet::protocol::TaskState::expired)));
            update.exec();
            if (sqlite3_changes(db.getHandle()) != 1) {
              throw std::runtime_error("task result state transition failed");
            }

            transaction.commit();
          },
          completion_executor, std::move(completion))) {
    boost::asio::post(completion_executor, [completion = std::move(completion)] {
      completion(std::make_exception_ptr(
          std::runtime_error("server database write actor is stopped")));
    });
  }
}

} // namespace zfleet::server
