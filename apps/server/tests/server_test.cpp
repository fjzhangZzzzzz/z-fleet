#include "config.h"
#include "database.h"
#include "http_handler.h"

#include <SQLiteCpp/SQLiteCpp.h>
#include <boost/beast/http.hpp>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>

namespace {

namespace http = boost::beast::http;

bool TableExists(const std::filesystem::path& database_path,
                 const std::string& table_name) {
  SQLite::Database db(database_path.string(), SQLite::OPEN_READONLY);
  SQLite::Statement query(
      db,
      "select name from sqlite_master where type = 'table' and name = ?");
  query.bind(1, table_name);
  return query.executeStep();
}

std::filesystem::path MakeTestRoot() {
  return std::filesystem::temp_directory_path() / "zfleet-server-tests" /
         "config-and-schema";
}

int CountRows(const std::filesystem::path& database_path,
              const std::string& table_name) {
  SQLite::Database db(database_path.string(), SQLite::OPEN_READONLY);
  SQLite::Statement query(db, "select count(*) from " + table_name);
  query.executeStep();
  return query.getColumn(0).getInt();
}

std::string ReadAgentLastSeen(const std::filesystem::path& database_path,
                              const std::string& agent_id) {
  SQLite::Database db(database_path.string(), SQLite::OPEN_READONLY);
  SQLite::Statement query(
      db, "select last_seen_at from agents where agent_id = ?");
  query.bind(1, agent_id);
  if (!query.executeStep()) {
    return {};
  }
  return query.getColumn(0).getString();
}

std::string ReadAuditField(const std::filesystem::path& database_path,
                           const std::string& request_id,
                           const std::string& column_name) {
  SQLite::Database db(database_path.string(), SQLite::OPEN_READONLY);
  SQLite::Statement query(
      db, "select " + column_name +
              " from audit_events where request_id = ? order by rowid desc limit 1");
  query.bind(1, request_id);
  if (!query.executeStep()) {
    return {};
  }
  return query.getColumn(0).getString();
}

std::string ReadTaskField(const std::filesystem::path& database_path,
                          const std::string& task_id,
                          const std::string& column_name) {
  SQLite::Database db(database_path.string(), SQLite::OPEN_READONLY);
  SQLite::Statement query(
      db, "select " + column_name + " from tasks where task_id = ?");
  query.bind(1, task_id);
  if (!query.executeStep()) {
    return {};
  }
  return query.getColumn(0).getString();
}

} // namespace

TEST_CASE("server config loads listen and database path from toml") {
  namespace fs = std::filesystem;

  const auto test_root = MakeTestRoot();
  fs::remove_all(test_root);
  fs::create_directories(test_root);

  const auto config_path = test_root / "server.toml";
  {
    std::ofstream config_stream(config_path);
    REQUIRE(config_stream);
    config_stream << "[server]\n";
    config_stream << "listen = \"127.0.0.1:18080\"\n";
    config_stream << "database_path = \"" << (test_root / "server.db").string()
                  << "\"\n";
    config_stream << "\n[log]\n";
    config_stream << "level = \"debug\"\n";
    config_stream << "file = \"" << (test_root / "server.log").string()
                  << "\"\n";
    config_stream << "enable_console = false\n";
  }

  const auto config = zfleet::server::LoadConfig(config_path);

  REQUIRE(config.listen == "127.0.0.1:18080");
  REQUIRE(config.database_path == test_root / "server.db");
  REQUIRE(config.log.level == zfleet::core::log::Level::kDebug);
  REQUIRE(config.log.file_path == test_root / "server.log");
  REQUIRE_FALSE(config.log.enable_console);

  fs::remove_all(test_root);
}

TEST_CASE("server database initializes schema and version") {
  namespace fs = std::filesystem;

  const auto test_root = MakeTestRoot();
  fs::remove_all(test_root);
  fs::create_directories(test_root);

  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();

  REQUIRE(fs::exists(database_path));
  REQUIRE(database.schema_version() == 2);
  REQUIRE(TableExists(database_path, "agents"));
  REQUIRE(TableExists(database_path, "heartbeats"));
  REQUIRE(TableExists(database_path, "asset_snapshots"));
  REQUIRE(TableExists(database_path, "audit_events"));
  REQUIRE(TableExists(database_path, "tasks"));
  REQUIRE(TableExists(database_path, "task_results"));

  fs::remove_all(test_root);
}

TEST_CASE("register request is accepted and persists agent") {
  namespace fs = std::filesystem;

  const auto test_root = MakeTestRoot();
  fs::remove_all(test_root);
  fs::create_directories(test_root);

  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  const zfleet::server::HttpHandler handler(&database);

  http::request<http::string_body> request{http::verb::post,
                                           "/v1/agents/register", 11};
  request.body() = R"json({
    "protocol_version": "v1",
    "request_id": "req-register",
    "agent_id": "agent-1",
    "occurred_at": "2026-05-14T10:00:00Z",
    "agent_version": "0.1.0",
    "hostname": "devbox-01",
    "os": "linux",
    "arch": "x86_64"
  })json";
  request.prepare_payload();

  const auto response = handler.Handle(request);
  const auto body = nlohmann::json::parse(response.body());

  REQUIRE(response.result() == http::status::ok);
  REQUIRE(body.at("status").get<std::string>() == "accepted");
  REQUIRE(database.AgentExists("agent-1"));
  REQUIRE(CountRows(database_path, "audit_events") == 1);
  REQUIRE(ReadAuditField(database_path, "req-register", "event_type") ==
          "agent.register");
  REQUIRE(ReadAuditField(database_path, "req-register", "payload_json")
              .find("\"status\":\"accepted\"") != std::string::npos);

  fs::remove_all(test_root);
}

TEST_CASE("invalid json returns structured invalid_json error") {
  namespace fs = std::filesystem;

  const auto test_root = MakeTestRoot();
  fs::remove_all(test_root);
  fs::create_directories(test_root);

  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  const zfleet::server::HttpHandler handler(&database);

  http::request<http::string_body> request{http::verb::post,
                                           "/v1/agents/register", 11};
  request.body() = "{invalid";
  request.prepare_payload();

  const auto response = handler.Handle(request);
  const auto body = nlohmann::json::parse(response.body());

  REQUIRE(response.result() == http::status::bad_request);
  REQUIRE(body.at("error_code").get<std::string>() == "invalid_json");
  REQUIRE(CountRows(database_path, "audit_events") == 0);

  fs::remove_all(test_root);
}

TEST_CASE("heartbeat path and body agent mismatch returns structured error") {
  namespace fs = std::filesystem;

  const auto test_root = MakeTestRoot();
  fs::remove_all(test_root);
  fs::create_directories(test_root);

  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  database.UpsertAgent(zfleet::protocol::RegistrationRequest{
      .protocol_version = "v1",
      .request_id = "seed",
      .agent_id = "agent-1",
      .occurred_at = "2026-05-14T10:00:00Z",
      .agent_version = "0.1.0",
      .hostname = "devbox-01",
      .os = "linux",
      .arch = "x86_64",
  });
  const zfleet::server::HttpHandler handler(&database);

  http::request<http::string_body> request{
      http::verb::post, "/v1/agents/agent-2/heartbeat", 11};
  request.body() = R"json({
    "protocol_version": "v1",
    "request_id": "req-heartbeat",
    "agent_id": "agent-1",
    "occurred_at": "2026-05-14T10:00:01Z",
    "agent_version": "0.1.0"
  })json";
  request.prepare_payload();

  const auto response = handler.Handle(request);
  const auto body = nlohmann::json::parse(response.body());

  REQUIRE(response.result() == http::status::bad_request);
  REQUIRE(body.at("error_code").get<std::string>() == "agent_id_mismatch");
  REQUIRE(CountRows(database_path, "audit_events") == 1);
  REQUIRE(ReadAuditField(database_path, "req-heartbeat", "event_type") ==
          "agent.heartbeat");
  REQUIRE(ReadAuditField(database_path, "req-heartbeat", "payload_json")
              .find("\"error_code\":\"agent_id_mismatch\"") !=
          std::string::npos);

  fs::remove_all(test_root);
}

TEST_CASE("heartbeat for unregistered agent returns not found error") {
  namespace fs = std::filesystem;

  const auto test_root = MakeTestRoot();
  fs::remove_all(test_root);
  fs::create_directories(test_root);

  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  const zfleet::server::HttpHandler handler(&database);

  http::request<http::string_body> request{
      http::verb::post, "/v1/agents/agent-1/heartbeat", 11};
  request.body() = R"json({
    "protocol_version": "v1",
    "request_id": "req-heartbeat",
    "agent_id": "agent-1",
    "occurred_at": "2026-05-14T10:00:01Z",
    "agent_version": "0.1.0"
  })json";
  request.prepare_payload();

  const auto response = handler.Handle(request);
  const auto body = nlohmann::json::parse(response.body());

  REQUIRE(response.result() == http::status::not_found);
  REQUIRE(body.at("error_code").get<std::string>() == "agent_not_registered");
  REQUIRE(CountRows(database_path, "audit_events") == 1);
  REQUIRE(ReadAuditField(database_path, "req-heartbeat", "payload_json")
              .find("\"error_code\":\"agent_not_registered\"") !=
          std::string::npos);

  fs::remove_all(test_root);
}

TEST_CASE("heartbeat and asset requests persist rows for registered agent") {
  namespace fs = std::filesystem;

  const auto test_root = MakeTestRoot();
  fs::remove_all(test_root);
  fs::create_directories(test_root);

  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  database.UpsertAgent(zfleet::protocol::RegistrationRequest{
      .protocol_version = "v1",
      .request_id = "seed",
      .agent_id = "agent-1",
      .occurred_at = "2026-05-14T10:00:00Z",
      .agent_version = "0.1.0",
      .hostname = "devbox-01",
      .os = "linux",
      .arch = "x86_64",
  });
  const zfleet::server::HttpHandler handler(&database);

  http::request<http::string_body> heartbeat{
      http::verb::post, "/v1/agents/agent-1/heartbeat", 11};
  heartbeat.body() = R"json({
    "protocol_version": "v1",
    "request_id": "req-heartbeat",
    "agent_id": "agent-1",
    "occurred_at": "2026-05-14T10:00:01Z",
    "agent_version": "0.1.0"
  })json";
  heartbeat.prepare_payload();
  const auto heartbeat_response = handler.Handle(heartbeat);

  http::request<http::string_body> assets{
      http::verb::post, "/v1/agents/agent-1/assets", 11};
  assets.body() = R"json({
    "protocol_version": "v1",
    "request_id": "req-assets",
    "agent_id": "agent-1",
    "occurred_at": "2026-05-14T10:00:02Z",
    "hostname": "devbox-01",
    "os": "linux",
    "arch": "x86_64",
    "agent_version": "0.1.0"
  })json";
  assets.prepare_payload();
  const auto assets_response = handler.Handle(assets);

  REQUIRE(heartbeat_response.result() == http::status::ok);
  REQUIRE(assets_response.result() == http::status::ok);
  REQUIRE(CountRows(database_path, "heartbeats") == 1);
  REQUIRE(CountRows(database_path, "asset_snapshots") == 1);
  REQUIRE(CountRows(database_path, "audit_events") == 2);
  REQUIRE(ReadAgentLastSeen(database_path, "agent-1") ==
          "2026-05-14T10:00:01Z");
  REQUIRE(ReadAuditField(database_path, "req-heartbeat", "event_type") ==
          "agent.heartbeat");
  REQUIRE(ReadAuditField(database_path, "req-assets", "event_type") ==
          "agent.asset_snapshot");

  fs::remove_all(test_root);
}

TEST_CASE("task poll returns idle when no queued task exists") {
  namespace fs = std::filesystem;

  const auto test_root = MakeTestRoot();
  fs::remove_all(test_root);
  fs::create_directories(test_root);

  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  database.UpsertAgent(zfleet::protocol::RegistrationRequest{
      .protocol_version = "v1",
      .request_id = "seed",
      .agent_id = "agent-1",
      .occurred_at = "2026-05-16T10:00:00Z",
      .agent_version = "0.1.0",
      .hostname = "devbox-01",
      .os = "linux",
      .arch = "x86_64",
  });
  const zfleet::server::HttpHandler handler(&database);

  http::request<http::string_body> request{http::verb::get,
                                           "/v1/agents/agent-1/tasks/poll", 11};
  request.set("X-Request-Id", "poll-idle");

  const auto response = handler.Handle(request);
  const auto body = nlohmann::json::parse(response.body());

  REQUIRE(response.result() == http::status::ok);
  REQUIRE(body.at("status").get<std::string>() == "idle");
  REQUIRE_FALSE(body.contains("task"));

  fs::remove_all(test_root);
}

TEST_CASE("task create request queues task and records audit") {
  namespace fs = std::filesystem;

  const auto test_root = MakeTestRoot();
  fs::remove_all(test_root);
  fs::create_directories(test_root);

  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  const zfleet::server::HttpHandler handler(&database);

  http::request<http::string_body> request{http::verb::post, "/v1/tasks", 11};
  request.body() = R"json({
    "protocol_version": "v1",
    "request_id": "task-create-1",
    "occurred_at": "2026-05-17T10:00:00Z",
    "task": {
      "protocol_version": "v1",
      "task_id": "task-1",
      "agent_id": "agent-1",
      "task_type": "collect_basic_inventory",
      "capability_level": "readonly",
      "created_at": "2026-05-17T10:00:00Z",
      "expires_at": "2099-05-17T10:05:00Z",
      "input": {}
    }
  })json";
  request.prepare_payload();

  const auto response = handler.Handle(request);
  const auto body = nlohmann::json::parse(response.body());

  REQUIRE(response.result() == http::status::ok);
  REQUIRE(body.at("status").get<std::string>() == "accepted");
  REQUIRE(CountRows(database_path, "tasks") == 1);
  REQUIRE(ReadTaskField(database_path, "task-1", "state") == "queued");
  REQUIRE(ReadAuditField(database_path, "task-create-1", "event_type") ==
          "task.queued");

  fs::remove_all(test_root);
}

TEST_CASE("task poll assigns queued task and records audit") {
  namespace fs = std::filesystem;

  const auto test_root = MakeTestRoot();
  fs::remove_all(test_root);
  fs::create_directories(test_root);

  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  database.UpsertAgent(zfleet::protocol::RegistrationRequest{
      .protocol_version = "v1",
      .request_id = "seed",
      .agent_id = "agent-1",
      .occurred_at = "2026-05-16T10:00:00Z",
      .agent_version = "0.1.0",
      .hostname = "devbox-01",
      .os = "linux",
      .arch = "x86_64",
  });
  database.EnqueueTask(zfleet::protocol::Task{
      .protocol_version = "v1",
      .task_id = "task-1",
      .agent_id = "agent-1",
      .task_type = zfleet::protocol::TaskType::collect_basic_inventory,
      .capability_level = zfleet::protocol::CapabilityLevel::readonly,
      .created_at = "2026-05-16T10:00:01Z",
      .expires_at = "2099-05-16T10:05:00Z",
      .input = zfleet::protocol::CollectBasicInventoryInput{},
  });
  const zfleet::server::HttpHandler handler(&database);

  http::request<http::string_body> request{http::verb::get,
                                           "/v1/agents/agent-1/tasks/poll", 11};
  request.set("X-Request-Id", "poll-assigned");

  const auto response = handler.Handle(request);
  const auto body = nlohmann::json::parse(response.body());

  REQUIRE(response.result() == http::status::ok);
  REQUIRE(body.at("status").get<std::string>() == "assigned");
  REQUIRE(body.at("task").at("task_id").get<std::string>() == "task-1");
  REQUIRE(ReadTaskField(database_path, "task-1", "state") == "assigned");
  REQUIRE(ReadAuditField(database_path, "poll-assigned", "event_type") ==
          "task.assigned");

  fs::remove_all(test_root);
}

TEST_CASE("task running request updates state and records audit") {
  namespace fs = std::filesystem;

  const auto test_root = MakeTestRoot();
  fs::remove_all(test_root);
  fs::create_directories(test_root);

  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  database.UpsertAgent(zfleet::protocol::RegistrationRequest{
      .protocol_version = "v1",
      .request_id = "seed",
      .agent_id = "agent-1",
      .occurred_at = "2026-05-17T10:00:00Z",
      .agent_version = "0.1.0",
      .hostname = "devbox-01",
      .os = "linux",
      .arch = "x86_64",
  });
  database.EnqueueTask(zfleet::protocol::Task{
      .protocol_version = "v1",
      .task_id = "task-1",
      .agent_id = "agent-1",
      .task_type = zfleet::protocol::TaskType::collect_basic_inventory,
      .capability_level = zfleet::protocol::CapabilityLevel::readonly,
      .created_at = "2026-05-17T10:00:01Z",
      .expires_at = "2099-05-17T10:05:00Z",
      .input = zfleet::protocol::CollectBasicInventoryInput{},
  });
  REQUIRE(database.ClaimNextTaskForAgent("agent-1", "2026-05-17T10:00:10Z")
              .has_value());
  const zfleet::server::HttpHandler handler(&database);

  http::request<http::string_body> request{http::verb::post,
                                           "/v1/tasks/task-1/running", 11};
  request.body() = R"json({
    "protocol_version": "v1",
    "request_id": "task-running-1",
    "task_id": "task-1",
    "agent_id": "agent-1",
    "task_type": "collect_basic_inventory",
    "occurred_at": "2026-05-17T10:00:20Z"
  })json";
  request.prepare_payload();

  const auto response = handler.Handle(request);
  const auto body = nlohmann::json::parse(response.body());

  REQUIRE(response.result() == http::status::ok);
  REQUIRE(body.at("status").get<std::string>() == "accepted");
  REQUIRE(ReadTaskField(database_path, "task-1", "state") == "running");
  REQUIRE(ReadAuditField(database_path, "task-running-1", "event_type") ==
          "task.running");

  fs::remove_all(test_root);
}

TEST_CASE("task result persists terminal state and result row") {
  namespace fs = std::filesystem;

  const auto test_root = MakeTestRoot();
  fs::remove_all(test_root);
  fs::create_directories(test_root);

  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  database.UpsertAgent(zfleet::protocol::RegistrationRequest{
      .protocol_version = "v1",
      .request_id = "seed",
      .agent_id = "agent-1",
      .occurred_at = "2026-05-16T10:00:00Z",
      .agent_version = "0.1.0",
      .hostname = "devbox-01",
      .os = "linux",
      .arch = "x86_64",
  });
  database.EnqueueTask(zfleet::protocol::Task{
      .protocol_version = "v1",
      .task_id = "task-1",
      .agent_id = "agent-1",
      .task_type = zfleet::protocol::TaskType::collect_basic_inventory,
      .capability_level = zfleet::protocol::CapabilityLevel::readonly,
      .created_at = "2026-05-16T10:00:01Z",
      .expires_at = "2099-05-16T10:05:00Z",
      .input = zfleet::protocol::CollectBasicInventoryInput{},
  });
  REQUIRE(database.ClaimNextTaskForAgent("agent-1", "2026-05-16T10:00:10Z")
              .has_value());
  const zfleet::server::HttpHandler handler(&database);

  http::request<http::string_body> request{http::verb::post,
                                           "/v1/tasks/task-1/result", 11};
  request.body() = R"json({
    "protocol_version": "v1",
    "request_id": "task-result-1",
    "task_id": "task-1",
    "agent_id": "agent-1",
    "task_type": "collect_basic_inventory",
    "occurred_at": "2026-05-16T10:00:30Z",
    "status": "succeeded",
    "result": {
      "hostname": "devbox-01",
      "os": "linux",
      "arch": "x86_64",
      "agent_version": "0.1.0"
    }
  })json";
  request.prepare_payload();

  const auto response = handler.Handle(request);
  const auto body = nlohmann::json::parse(response.body());

  REQUIRE(response.result() == http::status::ok);
  REQUIRE(body.at("status").get<std::string>() == "accepted");
  REQUIRE(ReadTaskField(database_path, "task-1", "state") == "succeeded");
  REQUIRE(CountRows(database_path, "task_results") == 1);
  REQUIRE(ReadAuditField(database_path, "task-result-1", "event_type") ==
          "task.succeeded");

  fs::remove_all(test_root);
}
