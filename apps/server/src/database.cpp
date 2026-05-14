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

} // namespace zfleet::server
