#include "database.h"

#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>

#include <mutex>
#include <stdexcept>
#include <string>
#include <variant>

namespace zfleet::server {
namespace {

constexpr int kSchemaVersion = 3;

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

std::string SerializeAgentEventBlob(const proto::AgentEvent& event) {
  return SerializeProto(event);
}

std::string SerializeTaskInputBlob(const zfleet::protocol::TaskInput& input) {
  return std::visit(
      [](const auto&) {
        proto::CollectBasicInventoryInput message;
        return SerializeProto(message);
      },
      input);
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

void ExecSchema(SQLite::Database& db) {
  db.exec(R"sql(
    create table if not exists agents (
      agent_id text primary key,
      first_seen_at text not null,
      last_seen_at text not null,
      platform text not null,
      status text not null
    )
  )sql");

  db.exec(R"sql(
    create table if not exists heartbeats (
      heartbeat_id integer primary key autoincrement,
      agent_id text not null,
      occurred_at text not null,
      agent_version text not null,
      event_blob blob not null
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

  db.exec("PRAGMA user_version = 3");
}

constexpr int kBusyTimeoutMs = 5000;

SQLite::Database OpenDatabase(const std::filesystem::path& database_path,
                              const int flags) {
  SQLite::Database db(database_path.string(), flags);
  db.exec("PRAGMA busy_timeout=" + std::to_string(kBusyTimeoutMs));
  return db;
}

} // namespace

ServerDatabase::ServerDatabase(std::filesystem::path database_path)
    : database_path_(std::move(database_path)) {}

void ServerDatabase::Initialize() {
  if (database_path_.empty()) {
    throw std::runtime_error("database path must not be empty");
  }

  const auto parent_path = database_path_.parent_path();
  if (!parent_path.empty()) {
    std::filesystem::create_directories(parent_path);
  }

  std::lock_guard lock(write_mutex_);
  auto db = OpenDatabase(database_path_,
                         SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
  db.exec("PRAGMA journal_mode=WAL");
  ExecSchema(db);
}

int ServerDatabase::schema_version() const {
  SQLite::Database db = OpenDatabase(database_path_, SQLite::OPEN_READONLY);
  SQLite::Statement query(db, "PRAGMA user_version");
  if (!query.executeStep()) {
    throw std::runtime_error("failed to read schema version");
  }

  return query.getColumn(0).getInt();
}

const std::filesystem::path& ServerDatabase::database_path() const noexcept {
  return database_path_;
}

bool ServerDatabase::AgentExists(const std::string& agent_id) const {
  SQLite::Database db = OpenDatabase(database_path_, SQLite::OPEN_READONLY);
  SQLite::Statement query(db, "select 1 from agents where agent_id = ?");
  query.bind(1, agent_id);
  return query.executeStep();
}

void ServerDatabase::UpsertAgent(
    const zfleet::protocol::AgentRegistration& request) {
  std::lock_guard lock(write_mutex_);
  SQLite::Database db = OpenDatabase(database_path_, SQLite::OPEN_READWRITE);
  SQLite::Statement statement(
      db,
      R"sql(
        insert into agents (agent_id, first_seen_at, last_seen_at, platform, status)
        values (?, ?, ?, ?, ?)
        on conflict(agent_id) do update set
          last_seen_at = excluded.last_seen_at,
          platform = excluded.platform,
          status = excluded.status
      )sql");
  statement.bind(1, request.agent_id);
  statement.bind(2, request.occurred_at);
  statement.bind(3, request.occurred_at);
  statement.bind(4, request.os);
  statement.bind(5, "online");
  statement.exec();
}

void ServerDatabase::RecordHeartbeat(
    const zfleet::protocol::AgentHeartbeat& request,
    const proto::AgentEvent& event) {
  const auto event_blob = SerializeAgentEventBlob(event);
  std::lock_guard lock(write_mutex_);
  SQLite::Database db = OpenDatabase(database_path_, SQLite::OPEN_READWRITE);
  SQLite::Transaction transaction(db);

  SQLite::Statement insert_statement(
      db,
      "insert into heartbeats (agent_id, occurred_at, agent_version, event_blob) "
      "values (?, ?, ?, ?)");
  insert_statement.bind(1, request.agent_id);
  insert_statement.bind(2, request.occurred_at);
  insert_statement.bind(3, request.agent_version);
  BindBlob(insert_statement, 4, event_blob);
  insert_statement.exec();

  SQLite::Statement update_statement(
      db, "update agents set last_seen_at = ?, status = ? where agent_id = ?");
  update_statement.bind(1, request.occurred_at);
  update_statement.bind(2, "online");
  update_statement.bind(3, request.agent_id);
  update_statement.exec();

  transaction.commit();
}

void ServerDatabase::RecordAssetSnapshot(
    const zfleet::protocol::AssetSnapshot& request,
    const proto::AgentEvent& event) {
  const auto event_blob = SerializeAgentEventBlob(event);
  std::lock_guard lock(write_mutex_);
  SQLite::Database db = OpenDatabase(database_path_, SQLite::OPEN_READWRITE);
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
          event_blob
        ) values (?, ?, ?, ?, ?, ?, ?, ?)
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
  BindBlob(statement, 8, event_blob);
  statement.exec();
}

void ServerDatabase::RecordAuditEvent(const zfleet::protocol::AuditEvent& event) {
  std::lock_guard lock(write_mutex_);
  SQLite::Database db = OpenDatabase(database_path_, SQLite::OPEN_READWRITE);
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
  statement.bind(4, std::string(zfleet::protocol::ToString(event.event_type)));
  statement.bind(5, event.request_id);
  statement.bind(6, event.payload_json);
  statement.exec();
}

void ServerDatabase::EnqueueTask(const zfleet::protocol::Task& task) {
  {
    std::lock_guard lock(write_mutex_);
    SQLite::Database db = OpenDatabase(database_path_, SQLite::OPEN_READWRITE);
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
    BindBlob(statement, 8, SerializeTaskInputBlob(task.input));
    statement.bind(9, std::string(zfleet::protocol::ToString(
                          zfleet::protocol::TaskState::queued)));
    statement.bind(10);
    statement.bind(11);
    statement.exec();
  }

  {
    std::lock_guard lock(task_queue_mutex_);
    ++task_queue_version_;
  }
  task_queue_cv_.notify_all();
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
    return task_queue_version_ != last_seen_version;
  });
  return task_queue_version_;
}

void ServerDatabase::MarkTaskRunning(
    const zfleet::protocol::TaskRunning& request) {
  std::lock_guard lock(write_mutex_);
  SQLite::Database db = OpenDatabase(database_path_, SQLite::OPEN_READWRITE);
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
}

std::optional<zfleet::protocol::Task> ServerDatabase::ClaimNextTaskForAgent(
    const std::string& agent_id,
    const std::string& assigned_at) {
  std::lock_guard lock(write_mutex_);
  SQLite::Database db = OpenDatabase(database_path_, SQLite::OPEN_READWRITE);
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
        order by created_at asc
        limit 1
      )sql");
  select.bind(1, agent_id);
  select.bind(2,
              std::string(
                  zfleet::protocol::ToString(zfleet::protocol::TaskState::queued)));
  select.bind(3, assigned_at);
  if (!select.executeStep()) {
    return std::nullopt;
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
    return std::nullopt;
  }
  transaction.commit();
  return task;
}

std::optional<StoredTask> ServerDatabase::FindTaskById(
    const std::string& task_id) const {
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
    return std::nullopt;
  }

  return StoredTask{
      .task = ParseStoredTask(query),
      .state = *zfleet::protocol::TaskStateFromString(
          query.getColumn(8).getString()),
      .assigned_at = NullableOptionalColumn(query, 9),
      .completed_at = NullableOptionalColumn(query, 10),
  };
}

void ServerDatabase::RecordTaskResult(
    const zfleet::protocol::TaskResult& request,
    const std::optional<std::string>& result_blob,
    const std::optional<std::string>& error_blob) {
  std::lock_guard lock(write_mutex_);
  SQLite::Database db = OpenDatabase(database_path_, SQLite::OPEN_READWRITE);
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
    insert.bind(
        8, std::string(zfleet::protocol::ToString(request.error->error_code)));
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

  transaction.commit();
}

} // namespace zfleet::server
