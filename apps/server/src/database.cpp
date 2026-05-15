#include "database.h"

#include <SQLiteCpp/SQLiteCpp.h>

#include <stdexcept>

namespace zfleet::server {
namespace {

constexpr int kSchemaVersion = 1;

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
      payload_json text not null
    )
  )sql");

  db.exec(R"sql(
    create table if not exists asset_snapshots (
      snapshot_id integer primary key autoincrement,
      agent_id text not null,
      occurred_at text not null,
      payload_json text not null
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

  db.exec("PRAGMA user_version = 1");
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

  SQLite::Database db(database_path_.string(),
                      SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
  ExecSchema(db);
}

int ServerDatabase::schema_version() const {
  SQLite::Database db(database_path_.string(), SQLite::OPEN_READONLY);
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
  SQLite::Database db(database_path_.string(), SQLite::OPEN_READONLY);
  SQLite::Statement query(db, "select 1 from agents where agent_id = ?");
  query.bind(1, agent_id);
  return query.executeStep();
}

void ServerDatabase::UpsertAgent(
    const zfleet::protocol::RegistrationRequest& request) {
  SQLite::Database db(database_path_.string(), SQLite::OPEN_READWRITE);
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
    const zfleet::protocol::HeartbeatRequest& request,
    const std::string& payload_json) {
  SQLite::Database db(database_path_.string(), SQLite::OPEN_READWRITE);
  SQLite::Transaction transaction(db);

  SQLite::Statement insert_statement(
      db,
      "insert into heartbeats (agent_id, occurred_at, payload_json) values (?, ?, ?)");
  insert_statement.bind(1, request.agent_id);
  insert_statement.bind(2, request.occurred_at);
  insert_statement.bind(3, payload_json);
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
    const zfleet::protocol::AssetSnapshotRequest& request,
    const std::string& payload_json) {
  SQLite::Database db(database_path_.string(), SQLite::OPEN_READWRITE);
  SQLite::Statement statement(
      db,
      "insert into asset_snapshots (agent_id, occurred_at, payload_json) values (?, ?, ?)");
  statement.bind(1, request.agent_id);
  statement.bind(2, request.occurred_at);
  statement.bind(3, payload_json);
  statement.exec();
}

void ServerDatabase::RecordAuditEvent(const zfleet::protocol::AuditEvent& event) {
  SQLite::Database db(database_path_.string(), SQLite::OPEN_READWRITE);
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

} // namespace zfleet::server
