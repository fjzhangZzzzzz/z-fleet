#include "config.h"
#include "database.h"
#include "http2_control_dispatcher.h"
#include "http2_control_service.h"
#include "http_handler.h"

#include "test_util.h"

#include "zfleet/protocol/json_codec.h"
#include "zfleet/protocol/v1/agent_control.pb.h"
#include "zfleet/transport/frame_codec.h"

#include <SQLiteCpp/SQLiteCpp.h>
#include <boost/beast/http.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

namespace http = boost::beast::http;

zfleet::protocol::StatusResponse ParseStatusResponse(
    const http::response<http::string_body>& response) {
  const auto parsed = zfleet::protocol::ParseStatusResponse(response.body());
  REQUIRE(std::holds_alternative<zfleet::protocol::StatusResponse>(parsed));
  return std::get<zfleet::protocol::StatusResponse>(parsed);
}

zfleet::protocol::ErrorResponse ParseErrorResponse(
    const http::response<http::string_body>& response) {
  const auto parsed = zfleet::protocol::ParseErrorResponse(response.body());
  REQUIRE(std::holds_alternative<zfleet::protocol::ErrorResponse>(parsed));
  return std::get<zfleet::protocol::ErrorResponse>(parsed);
}

zfleet::protocol::TaskPollResponse ParseTaskPollResponse(
    const http::response<http::string_body>& response) {
  const auto parsed = zfleet::protocol::ParseTaskPollResponse(response.body());
  REQUIRE(std::holds_alternative<zfleet::protocol::TaskPollResponse>(parsed));
  return std::get<zfleet::protocol::TaskPollResponse>(parsed);
}

bool TableExists(const std::filesystem::path& database_path,
                 const std::string& table_name) {
  SQLite::Database db(database_path.string(), SQLite::OPEN_READONLY);
  SQLite::Statement query(
      db,
      "select name from sqlite_master where type = 'table' and name = ?");
  query.bind(1, table_name);
  return query.executeStep();
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

  const zfleet::test::ScopedTestDir test_root("server");

  const auto config_path = test_root / "server.toml";
  {
    std::ofstream config_stream(config_path);
    REQUIRE(config_stream);
    config_stream << "[server]\n";
    config_stream << "listen = \"127.0.0.1:18080\"\n";
    config_stream << "control_listen = \"127.0.0.1:18081\"\n";
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
  REQUIRE(config.control_listen == "127.0.0.1:18081");
  REQUIRE(config.database_path == test_root / "server.db");
  REQUIRE(config.log.level == zfleet::core::log::Level::kDebug);
  REQUIRE(config.log.file_path == test_root / "server.log");
  REQUIRE_FALSE(config.log.enable_console);
}

TEST_CASE("server database initializes schema and version") {
  namespace fs = std::filesystem;

  const zfleet::test::ScopedTestDir test_root("server");

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

}

TEST_CASE("register request is accepted and persists agent") {
  namespace fs = std::filesystem;

  const zfleet::test::ScopedTestDir test_root("server");

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
  const auto body = ParseStatusResponse(response);

  REQUIRE(response.result() == http::status::ok);
  REQUIRE(body.status == "accepted");
  REQUIRE(database.AgentExists("agent-1"));
  REQUIRE(CountRows(database_path, "audit_events") == 1);
  REQUIRE(ReadAuditField(database_path, "req-register", "event_type") ==
          "agent.register");
  REQUIRE(ReadAuditField(database_path, "req-register", "payload_json")
              .find("\"status\":\"accepted\"") != std::string::npos);

}

TEST_CASE("http2 control service registers agent and stores heartbeat") {
  namespace proto = zfleet::protocol::v1;

  const zfleet::test::ScopedTestDir test_root("server");

  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  const zfleet::server::Http2ControlService service(&database);

  proto::AgentEvent registration;
  registration.set_protocol_version("v1");
  registration.set_message_id("http2-register");
  registration.set_agent_id("agent-1");
  registration.set_occurred_at("2026-05-21T10:00:00Z");
  auto* register_payload = registration.mutable_register_();
  register_payload->set_agent_version("0.1.0");
  register_payload->set_hostname("devbox-01");
  register_payload->set_os("linux");
  register_payload->set_arch("x86_64");

  const auto register_result = service.HandleAgentEvent(registration);

  proto::AgentEvent heartbeat;
  heartbeat.set_protocol_version("v1");
  heartbeat.set_message_id("http2-heartbeat");
  heartbeat.set_agent_id("agent-1");
  heartbeat.set_occurred_at("2026-05-21T10:00:05Z");
  heartbeat.mutable_heartbeat()->set_agent_version("0.1.0");

  const auto heartbeat_result = service.HandleAgentEvent(heartbeat);

  REQUIRE(register_result.status ==
          zfleet::server::ControlEventStatus::kAccepted);
  REQUIRE(register_result.message == "accepted");
  REQUIRE(heartbeat_result.status ==
          zfleet::server::ControlEventStatus::kAccepted);
  REQUIRE(heartbeat_result.message == "ok");
  REQUIRE(database.AgentExists("agent-1"));
  REQUIRE(CountRows(database_path, "heartbeats") == 1);
  REQUIRE(CountRows(database_path, "audit_events") == 2);
  REQUIRE(ReadAgentLastSeen(database_path, "agent-1") ==
          "2026-05-21T10:00:05Z");
  REQUIRE(ReadAuditField(database_path, "http2-register", "event_type") ==
          "agent.register");
  REQUIRE(ReadAuditField(database_path, "http2-heartbeat", "event_type") ==
          "agent.heartbeat");
  REQUIRE(ReadAuditField(database_path, "http2-heartbeat", "payload_json")
              .find("\"status\":\"ok\"") != std::string::npos);

}

TEST_CASE("http2 control service rejects invalid and unregistered events") {
  namespace proto = zfleet::protocol::v1;

  const zfleet::test::ScopedTestDir test_root("server");

  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  const zfleet::server::Http2ControlService service(&database);

  proto::AgentEvent missing_payload;
  missing_payload.set_protocol_version("v1");
  missing_payload.set_message_id("http2-missing-payload");
  missing_payload.set_agent_id("agent-1");
  missing_payload.set_occurred_at("2026-05-21T10:00:00Z");

  const auto missing_payload_result = service.HandleAgentEvent(missing_payload);

  proto::AgentEvent heartbeat;
  heartbeat.set_protocol_version("v1");
  heartbeat.set_message_id("http2-unregistered-heartbeat");
  heartbeat.set_agent_id("agent-1");
  heartbeat.set_occurred_at("2026-05-21T10:00:05Z");
  heartbeat.mutable_heartbeat()->set_agent_version("0.1.0");

  const auto heartbeat_result = service.HandleAgentEvent(heartbeat);

  REQUIRE(missing_payload_result.status ==
          zfleet::server::ControlEventStatus::kInvalidArgument);
  REQUIRE(missing_payload_result.message == "event payload must be set");
  REQUIRE(heartbeat_result.status ==
          zfleet::server::ControlEventStatus::kNotFound);
  REQUIRE(heartbeat_result.message == "agent not registered");
  REQUIRE(CountRows(database_path, "heartbeats") == 0);
  REQUIRE(CountRows(database_path, "audit_events") == 1);
  REQUIRE(ReadAuditField(
              database_path, "http2-unregistered-heartbeat", "payload_json")
              .find("\"error_code\":\"agent_not_registered\"") !=
          std::string::npos);

}

TEST_CASE("http2 control dispatcher decodes framed protobuf event stream") {
  namespace proto = zfleet::protocol::v1;

  const zfleet::test::ScopedTestDir test_root("server");

  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  zfleet::server::Http2ControlService service(&database);
  zfleet::server::Http2ControlDispatcher dispatcher(&service);

  proto::AgentEvent registration;
  registration.set_protocol_version("v1");
  registration.set_message_id("framed-register");
  registration.set_agent_id("agent-1");
  registration.set_occurred_at("2026-05-21T11:00:00Z");
  auto* register_payload = registration.mutable_register_();
  register_payload->set_agent_version("0.1.0");
  register_payload->set_hostname("devbox-01");
  register_payload->set_os("linux");
  register_payload->set_arch("x86_64");

  proto::AgentEvent heartbeat;
  heartbeat.set_protocol_version("v1");
  heartbeat.set_message_id("framed-heartbeat");
  heartbeat.set_agent_id("agent-1");
  heartbeat.set_occurred_at("2026-05-21T11:00:05Z");
  heartbeat.mutable_heartbeat()->set_agent_version("0.1.0");

  std::string registration_bytes;
  std::string heartbeat_bytes;
  REQUIRE(registration.SerializeToString(&registration_bytes));
  REQUIRE(heartbeat.SerializeToString(&heartbeat_bytes));

  const auto registration_frame = zfleet::transport::EncodeFrame(
      std::span<const std::uint8_t>{
          reinterpret_cast<const std::uint8_t*>(registration_bytes.data()),
          registration_bytes.size()});
  const auto heartbeat_frame = zfleet::transport::EncodeFrame(
      std::span<const std::uint8_t>{
          reinterpret_cast<const std::uint8_t*>(heartbeat_bytes.data()),
          heartbeat_bytes.size()});

  std::vector<std::uint8_t> stream_bytes;
  stream_bytes.insert(stream_bytes.end(), registration_frame.begin(),
                      registration_frame.end());
  stream_bytes.insert(stream_bytes.end(), heartbeat_frame.begin(),
                      heartbeat_frame.end());

  const auto partial_results = dispatcher.PushEventBytes(
      std::span<const std::uint8_t>{stream_bytes.data(), 7});
  const auto complete_results = dispatcher.PushEventBytes(
      std::span<const std::uint8_t>{stream_bytes.data() + 7,
                                    stream_bytes.size() - 7});

  REQUIRE(partial_results.empty());
  REQUIRE(complete_results.size() == 2);
  REQUIRE(complete_results[0].status ==
          zfleet::server::ControlEventStatus::kAccepted);
  REQUIRE(complete_results[1].status ==
          zfleet::server::ControlEventStatus::kAccepted);
  REQUIRE(CountRows(database_path, "heartbeats") == 1);
  REQUIRE(ReadAgentLastSeen(database_path, "agent-1") ==
          "2026-05-21T11:00:05Z");

}

TEST_CASE("invalid json returns structured invalid_json error") {
  namespace fs = std::filesystem;

  const zfleet::test::ScopedTestDir test_root("server");

  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  const zfleet::server::HttpHandler handler(&database);

  http::request<http::string_body> request{http::verb::post,
                                           "/v1/agents/register", 11};
  request.body() = "{invalid";
  request.prepare_payload();

  const auto response = handler.Handle(request);
  const auto body = ParseErrorResponse(response);

  REQUIRE(response.result() == http::status::bad_request);
  REQUIRE(body.error_code == zfleet::protocol::ErrorCode::invalid_json);
  REQUIRE(CountRows(database_path, "audit_events") == 0);

}

TEST_CASE("heartbeat path and body agent mismatch returns structured error") {
  namespace fs = std::filesystem;

  const zfleet::test::ScopedTestDir test_root("server");

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
  const auto body = ParseErrorResponse(response);

  REQUIRE(response.result() == http::status::bad_request);
  REQUIRE(body.error_code == zfleet::protocol::ErrorCode::agent_id_mismatch);
  REQUIRE(CountRows(database_path, "audit_events") == 1);
  REQUIRE(ReadAuditField(database_path, "req-heartbeat", "event_type") ==
          "agent.heartbeat");
  REQUIRE(ReadAuditField(database_path, "req-heartbeat", "payload_json")
              .find("\"error_code\":\"agent_id_mismatch\"") !=
          std::string::npos);

}

TEST_CASE("heartbeat for unregistered agent returns not found error") {
  namespace fs = std::filesystem;

  const zfleet::test::ScopedTestDir test_root("server");

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
  const auto body = ParseErrorResponse(response);

  REQUIRE(response.result() == http::status::not_found);
  REQUIRE(body.error_code == zfleet::protocol::ErrorCode::agent_not_registered);
  REQUIRE(CountRows(database_path, "audit_events") == 1);
  REQUIRE(ReadAuditField(database_path, "req-heartbeat", "payload_json")
              .find("\"error_code\":\"agent_not_registered\"") !=
          std::string::npos);

}

TEST_CASE("heartbeat and asset requests persist rows for registered agent") {
  namespace fs = std::filesystem;

  const zfleet::test::ScopedTestDir test_root("server");

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

}

TEST_CASE("task poll returns idle when no queued task exists") {
  namespace fs = std::filesystem;

  const zfleet::test::ScopedTestDir test_root("server");

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
  const auto body = ParseTaskPollResponse(response);

  REQUIRE(response.result() == http::status::ok);
  REQUIRE(body.status == zfleet::protocol::TaskPollStatus::idle);
  REQUIRE_FALSE(body.task.has_value());

}

TEST_CASE("task poll for unregistered agent returns not found error") {
  namespace fs = std::filesystem;

  const zfleet::test::ScopedTestDir test_root("server");

  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  const zfleet::server::HttpHandler handler(&database);

  http::request<http::string_body> request{http::verb::get,
                                           "/v1/agents/agent-missing/tasks/poll", 11};
  request.set("X-Request-Id", "poll-missing-agent");

  const auto response = handler.Handle(request);
  const auto body = ParseErrorResponse(response);

  REQUIRE(response.result() == http::status::not_found);
  REQUIRE(body.error_code == zfleet::protocol::ErrorCode::agent_not_registered);

}

TEST_CASE("task create request queues task and records audit") {
  namespace fs = std::filesystem;

  const zfleet::test::ScopedTestDir test_root("server");

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
  const auto body = ParseStatusResponse(response);

  REQUIRE(response.result() == http::status::ok);
  REQUIRE(body.status == "accepted");
  REQUIRE(CountRows(database_path, "tasks") == 1);
  REQUIRE(ReadTaskField(database_path, "task-1", "state") == "queued");
  REQUIRE(ReadAuditField(database_path, "task-create-1", "event_type") ==
          "task.queued");

}

TEST_CASE("task create request rejects empty task_id and records failure audit") {
  namespace fs = std::filesystem;

  const zfleet::test::ScopedTestDir test_root("server");

  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  const zfleet::server::HttpHandler handler(&database);

  http::request<http::string_body> request{http::verb::post, "/v1/tasks", 11};
  request.body() = R"json({
    "protocol_version": "v1",
    "request_id": "task-create-empty-id",
    "occurred_at": "2026-05-17T10:00:00Z",
    "task": {
      "protocol_version": "v1",
      "task_id": "",
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
  const auto body = ParseErrorResponse(response);

  REQUIRE(response.result() == http::status::bad_request);
  REQUIRE(body.error_code == zfleet::protocol::ErrorCode::missing_required_field);
  REQUIRE(CountRows(database_path, "tasks") == 0);
  REQUIRE(ReadAuditField(database_path, "task-create-empty-id", "event_type") ==
          "task.queued");
  REQUIRE(ReadAuditField(database_path, "task-create-empty-id", "payload_json")
              .find("\"error_code\":\"missing_required_field\"") !=
          std::string::npos);

}

TEST_CASE("task create request rejects non-readonly capability and records failure audit") {
  namespace fs = std::filesystem;

  const zfleet::test::ScopedTestDir test_root("server");

  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  const zfleet::server::HttpHandler handler(&database);

  http::request<http::string_body> request{http::verb::post, "/v1/tasks", 11};
  request.body() = R"json({
    "protocol_version": "v1",
    "request_id": "task-create-shell",
    "occurred_at": "2026-05-17T10:00:00Z",
    "task": {
      "protocol_version": "v1",
      "task_id": "task-shell-1",
      "agent_id": "agent-1",
      "task_type": "collect_basic_inventory",
      "capability_level": "shell",
      "created_at": "2026-05-17T10:00:00Z",
      "expires_at": "2099-05-17T10:05:00Z",
      "input": {}
    }
  })json";
  request.prepare_payload();

  const auto response = handler.Handle(request);
  const auto body = ParseErrorResponse(response);

  REQUIRE(response.result() == http::status::forbidden);
  REQUIRE(body.error_code == zfleet::protocol::ErrorCode::capability_not_allowed);
  REQUIRE(CountRows(database_path, "tasks") == 0);
  REQUIRE(ReadAuditField(database_path, "task-create-shell", "event_type") ==
          "task.queued");
  REQUIRE(ReadAuditField(database_path, "task-create-shell", "payload_json")
              .find("\"error_code\":\"capability_not_allowed\"") !=
          std::string::npos);

}

TEST_CASE("task create request rejects unsupported task type and records failure audit") {
  namespace fs = std::filesystem;

  const zfleet::test::ScopedTestDir test_root("server");

  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  const zfleet::server::HttpHandler handler(&database);

  http::request<http::string_body> request{http::verb::post, "/v1/tasks", 11};
  request.body() = R"json({
    "protocol_version": "v1",
    "request_id": "task-create-unknown-type",
    "occurred_at": "2026-05-17T10:00:00Z",
    "task": {
      "protocol_version": "v1",
      "task_id": "task-unknown-1",
      "agent_id": "agent-1",
      "task_type": "collect_everything",
      "capability_level": "readonly",
      "created_at": "2026-05-17T10:00:00Z",
      "expires_at": "2099-05-17T10:05:00Z",
      "input": {}
    }
  })json";
  request.prepare_payload();

  const auto response = handler.Handle(request);
  const auto body = ParseErrorResponse(response);

  REQUIRE(response.result() == http::status::bad_request);
  REQUIRE(body.error_code == zfleet::protocol::ErrorCode::unsupported_task_type);
  REQUIRE(CountRows(database_path, "tasks") == 0);
  REQUIRE(ReadAuditField(database_path, "task-create-unknown-type", "event_type") ==
          "task.queued");
  REQUIRE(ReadAuditField(database_path, "task-create-unknown-type", "payload_json")
              .find("\"error_code\":\"unsupported_task_type\"") !=
          std::string::npos);

}

TEST_CASE("task create request rejects invalid capability field and records failure audit") {
  namespace fs = std::filesystem;

  const zfleet::test::ScopedTestDir test_root("server");

  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  const zfleet::server::HttpHandler handler(&database);

  http::request<http::string_body> request{http::verb::post, "/v1/tasks", 11};
  request.body() = R"json({
    "protocol_version": "v1",
    "request_id": "task-create-invalid-capability",
    "occurred_at": "2026-05-17T10:00:00Z",
    "task": {
      "protocol_version": "v1",
      "task_id": "task-invalid-capability-1",
      "agent_id": "agent-1",
      "task_type": "collect_basic_inventory",
      "capability_level": "readwrite",
      "created_at": "2026-05-17T10:00:00Z",
      "expires_at": "2099-05-17T10:05:00Z",
      "input": {}
    }
  })json";
  request.prepare_payload();

  const auto response = handler.Handle(request);
  const auto body = ParseErrorResponse(response);

  REQUIRE(response.result() == http::status::bad_request);
  REQUIRE(body.error_code == zfleet::protocol::ErrorCode::invalid_field_type);
  REQUIRE(CountRows(database_path, "tasks") == 0);
  REQUIRE(
      ReadAuditField(database_path, "task-create-invalid-capability", "event_type") ==
      "task.queued");
  REQUIRE(
      ReadAuditField(database_path, "task-create-invalid-capability", "payload_json")
          .find("\"error_code\":\"invalid_field_type\"") != std::string::npos);

}

TEST_CASE("task create request rejects invalid task input and records failure audit") {
  namespace fs = std::filesystem;

  const zfleet::test::ScopedTestDir test_root("server");

  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  const zfleet::server::HttpHandler handler(&database);

  http::request<http::string_body> request{http::verb::post, "/v1/tasks", 11};
  request.body() = R"json({
    "protocol_version": "v1",
    "request_id": "task-create-invalid-input",
    "occurred_at": "2026-05-17T10:00:00Z",
    "task": {
      "protocol_version": "v1",
      "task_id": "task-invalid-input-1",
      "agent_id": "agent-1",
      "task_type": "collect_basic_inventory",
      "capability_level": "readonly",
      "created_at": "2026-05-17T10:00:00Z",
      "expires_at": "2099-05-17T10:05:00Z",
      "input": []
    }
  })json";
  request.prepare_payload();

  const auto response = handler.Handle(request);
  const auto body = ParseErrorResponse(response);

  REQUIRE(response.result() == http::status::bad_request);
  REQUIRE(body.error_code == zfleet::protocol::ErrorCode::invalid_field_type);
  REQUIRE(CountRows(database_path, "tasks") == 0);
  REQUIRE(ReadAuditField(database_path, "task-create-invalid-input", "event_type") ==
          "task.queued");
  REQUIRE(ReadAuditField(database_path, "task-create-invalid-input", "payload_json")
              .find("\"error_code\":\"invalid_field_type\"") !=
          std::string::npos);

}

TEST_CASE("task poll assigns queued task and records audit") {
  namespace fs = std::filesystem;

  const zfleet::test::ScopedTestDir test_root("server");

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
  const auto body = ParseTaskPollResponse(response);

  REQUIRE(response.result() == http::status::ok);
  REQUIRE(body.status == zfleet::protocol::TaskPollStatus::assigned);
  REQUIRE(body.task.has_value());
  REQUIRE(body.task->task_id == "task-1");
  REQUIRE(ReadTaskField(database_path, "task-1", "state") == "assigned");
  REQUIRE(ReadAuditField(database_path, "poll-assigned", "event_type") ==
          "task.assigned");

}

TEST_CASE("task running request updates state and records audit") {
  namespace fs = std::filesystem;

  const zfleet::test::ScopedTestDir test_root("server");

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
  const auto body = ParseStatusResponse(response);

  REQUIRE(response.result() == http::status::ok);
  REQUIRE(body.status == "accepted");
  REQUIRE(ReadTaskField(database_path, "task-1", "state") == "running");
  REQUIRE(ReadAuditField(database_path, "task-running-1", "event_type") ==
          "task.running");

}

TEST_CASE("task running request returns not found for missing task") {
  namespace fs = std::filesystem;

  const zfleet::test::ScopedTestDir test_root("server");

  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  const zfleet::server::HttpHandler handler(&database);

  http::request<http::string_body> request{http::verb::post,
                                           "/v1/tasks/task-missing/running", 11};
  request.body() = R"json({
    "protocol_version": "v1",
    "request_id": "task-running-missing",
    "task_id": "task-missing",
    "agent_id": "agent-1",
    "task_type": "collect_basic_inventory",
    "occurred_at": "2026-05-17T10:00:20Z"
  })json";
  request.prepare_payload();

  const auto response = handler.Handle(request);
  const auto body = ParseErrorResponse(response);

  REQUIRE(response.result() == http::status::not_found);
  REQUIRE(body.error_code == zfleet::protocol::ErrorCode::task_not_found);

}

TEST_CASE("task running request rejects agent mismatch") {
  namespace fs = std::filesystem;

  const zfleet::test::ScopedTestDir test_root("server");

  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
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
  const zfleet::server::HttpHandler handler(&database);

  http::request<http::string_body> request{http::verb::post,
                                           "/v1/tasks/task-1/running", 11};
  request.body() = R"json({
    "protocol_version": "v1",
    "request_id": "task-running-agent-mismatch",
    "task_id": "task-1",
    "agent_id": "agent-2",
    "task_type": "collect_basic_inventory",
    "occurred_at": "2026-05-17T10:00:20Z"
  })json";
  request.prepare_payload();

  const auto response = handler.Handle(request);
  const auto body = ParseErrorResponse(response);

  REQUIRE(response.result() == http::status::bad_request);
  REQUIRE(body.error_code == zfleet::protocol::ErrorCode::task_agent_mismatch);

}

TEST_CASE("task running request rejects path and body task_id mismatch") {
  namespace fs = std::filesystem;

  const zfleet::test::ScopedTestDir test_root("server");

  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  const zfleet::server::HttpHandler handler(&database);

  http::request<http::string_body> request{http::verb::post,
                                           "/v1/tasks/task-1/running", 11};
  request.body() = R"json({
    "protocol_version": "v1",
    "request_id": "task-running-id-mismatch",
    "task_id": "task-2",
    "agent_id": "agent-1",
    "task_type": "collect_basic_inventory",
    "occurred_at": "2026-05-17T10:00:20Z"
  })json";
  request.prepare_payload();

  const auto response = handler.Handle(request);
  const auto body = ParseErrorResponse(response);

  REQUIRE(response.result() == http::status::bad_request);
  REQUIRE(body.error_code == zfleet::protocol::ErrorCode::task_result_invalid);

}

TEST_CASE("task running request rejects task type mismatch") {
  namespace fs = std::filesystem;

  const zfleet::test::ScopedTestDir test_root("server");

  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
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
  const zfleet::server::HttpHandler handler(&database);

  http::request<http::string_body> request{http::verb::post,
                                           "/v1/tasks/task-1/running", 11};
  request.body() = R"json({
    "protocol_version": "v1",
    "request_id": "task-running-type-mismatch",
    "task_id": "task-1",
    "agent_id": "agent-1",
    "task_type": "unknown_task_type_for_test",
    "occurred_at": "2026-05-17T10:00:20Z"
  })json";
  request.prepare_payload();

  const auto response = handler.Handle(request);
  const auto body = ParseErrorResponse(response);

  REQUIRE(response.result() == http::status::bad_request);
  REQUIRE(body.error_code == zfleet::protocol::ErrorCode::invalid_field_type);

}

TEST_CASE("task running request rejects already finished task") {
  namespace fs = std::filesystem;

  const zfleet::test::ScopedTestDir test_root("server");

  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
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
  database.RecordTaskResult(
      zfleet::protocol::TaskResultRequest{
          .protocol_version = "v1",
          .request_id = "seed-terminal",
          .task_id = "task-1",
          .agent_id = "agent-1",
          .task_type = zfleet::protocol::TaskType::collect_basic_inventory,
          .occurred_at = "2026-05-17T10:00:30Z",
          .status = zfleet::protocol::TaskExecutionStatus::succeeded,
          .result = zfleet::protocol::TaskResultData{
              zfleet::protocol::CollectBasicInventoryResult{
                  .hostname = "devbox-01",
                  .os = "linux",
                  .arch = "x86_64",
                  .agent_version = "0.1.0",
              }},
          .error = std::nullopt,
      },
      std::optional<std::string>{R"json({"hostname":"devbox-01","os":"linux","arch":"x86_64","agent_version":"0.1.0"})json"},
      std::nullopt);
  const zfleet::server::HttpHandler handler(&database);

  http::request<http::string_body> request{http::verb::post,
                                           "/v1/tasks/task-1/running", 11};
  request.body() = R"json({
    "protocol_version": "v1",
    "request_id": "task-running-finished",
    "task_id": "task-1",
    "agent_id": "agent-1",
    "task_type": "collect_basic_inventory",
    "occurred_at": "2026-05-17T10:00:40Z"
  })json";
  request.prepare_payload();

  const auto response = handler.Handle(request);
  const auto body = ParseErrorResponse(response);

  REQUIRE(response.result() == http::status::conflict);
  REQUIRE(body.error_code == zfleet::protocol::ErrorCode::task_already_finished);

}

TEST_CASE("task result persists terminal state and result row") {
  namespace fs = std::filesystem;

  const zfleet::test::ScopedTestDir test_root("server");

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
  const auto body = ParseStatusResponse(response);

  REQUIRE(response.result() == http::status::ok);
  REQUIRE(body.status == "accepted");
  REQUIRE(ReadTaskField(database_path, "task-1", "state") == "succeeded");
  REQUIRE(CountRows(database_path, "task_results") == 1);
  REQUIRE(ReadAuditField(database_path, "task-result-1", "event_type") ==
          "task.succeeded");

}

TEST_CASE("task result rejects succeeded status without result") {
  namespace fs = std::filesystem;

  const zfleet::test::ScopedTestDir test_root("server");

  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
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

  http::request<http::string_body> request{http::verb::post,
                                           "/v1/tasks/task-1/result", 11};
  request.body() = R"json({
    "protocol_version": "v1",
    "request_id": "task-result-missing-result",
    "task_id": "task-1",
    "agent_id": "agent-1",
    "task_type": "collect_basic_inventory",
    "occurred_at": "2026-05-16T10:00:30Z",
    "status": "succeeded"
  })json";
  request.prepare_payload();

  const auto response = handler.Handle(request);
  const auto body = ParseErrorResponse(response);

  REQUIRE(response.result() == http::status::bad_request);
  REQUIRE(body.error_code == zfleet::protocol::ErrorCode::task_result_invalid);
  REQUIRE(CountRows(database_path, "task_results") == 0);

}

TEST_CASE("task result rejects path and body task_id mismatch") {
  namespace fs = std::filesystem;

  const zfleet::test::ScopedTestDir test_root("server");

  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  const zfleet::server::HttpHandler handler(&database);

  http::request<http::string_body> request{http::verb::post,
                                           "/v1/tasks/task-1/result", 11};
  request.body() = R"json({
    "protocol_version": "v1",
    "request_id": "task-result-id-mismatch",
    "task_id": "task-2",
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
  const auto body = ParseErrorResponse(response);

  REQUIRE(response.result() == http::status::bad_request);
  REQUIRE(body.error_code == zfleet::protocol::ErrorCode::task_result_invalid);

}

TEST_CASE("task result rejects task type mismatch") {
  namespace fs = std::filesystem;

  const zfleet::test::ScopedTestDir test_root("server");

  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
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

  http::request<http::string_body> request{http::verb::post,
                                           "/v1/tasks/task-1/result", 11};
  request.body() = R"json({
    "protocol_version": "v1",
    "request_id": "task-result-type-mismatch",
    "task_id": "task-1",
    "agent_id": "agent-1",
    "task_type": "unknown_task_type_for_test",
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
  const auto body = ParseErrorResponse(response);

  REQUIRE(response.result() == http::status::bad_request);
  REQUIRE(body.error_code == zfleet::protocol::ErrorCode::invalid_field_type);

}

TEST_CASE("task result rejects failed status without error") {
  namespace fs = std::filesystem;

  const zfleet::test::ScopedTestDir test_root("server");

  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
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

  http::request<http::string_body> request{http::verb::post,
                                           "/v1/tasks/task-1/result", 11};
  request.body() = R"json({
    "protocol_version": "v1",
    "request_id": "task-result-missing-error",
    "task_id": "task-1",
    "agent_id": "agent-1",
    "task_type": "collect_basic_inventory",
    "occurred_at": "2026-05-16T10:00:30Z",
    "status": "failed"
  })json";
  request.prepare_payload();

  const auto response = handler.Handle(request);
  const auto body = ParseErrorResponse(response);

  REQUIRE(response.result() == http::status::bad_request);
  REQUIRE(body.error_code == zfleet::protocol::ErrorCode::task_result_invalid);
  REQUIRE(CountRows(database_path, "task_results") == 0);

}

TEST_CASE("task result rejects expired status without error") {
  namespace fs = std::filesystem;

  const zfleet::test::ScopedTestDir test_root("server");

  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
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

  http::request<http::string_body> request{http::verb::post,
                                           "/v1/tasks/task-1/result", 11};
  request.body() = R"json({
    "protocol_version": "v1",
    "request_id": "task-result-expired-missing-error",
    "task_id": "task-1",
    "agent_id": "agent-1",
    "task_type": "collect_basic_inventory",
    "occurred_at": "2026-05-16T10:00:30Z",
    "status": "expired"
  })json";
  request.prepare_payload();

  const auto response = handler.Handle(request);
  const auto body = ParseErrorResponse(response);

  REQUIRE(response.result() == http::status::bad_request);
  REQUIRE(body.error_code == zfleet::protocol::ErrorCode::task_result_invalid);
  REQUIRE(CountRows(database_path, "task_results") == 0);

}

TEST_CASE("task result rejects succeeded status with error") {
  namespace fs = std::filesystem;

  const zfleet::test::ScopedTestDir test_root("server");

  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
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

  http::request<http::string_body> request{http::verb::post,
                                           "/v1/tasks/task-1/result", 11};
  request.body() = R"json({
    "protocol_version": "v1",
    "request_id": "task-result-succeeded-with-error",
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
    },
    "error": {
      "error_code": "task_execution_failed",
      "message": "should not be present",
      "retryable": false
    }
  })json";
  request.prepare_payload();

  const auto response = handler.Handle(request);
  const auto body = ParseErrorResponse(response);

  REQUIRE(response.result() == http::status::bad_request);
  REQUIRE(body.error_code == zfleet::protocol::ErrorCode::task_result_invalid);
  REQUIRE(CountRows(database_path, "task_results") == 0);

}

TEST_CASE("task result returns not found for missing task") {
  namespace fs = std::filesystem;

  const zfleet::test::ScopedTestDir test_root("server");

  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  const zfleet::server::HttpHandler handler(&database);

  http::request<http::string_body> request{http::verb::post,
                                           "/v1/tasks/task-missing/result", 11};
  request.body() = R"json({
    "protocol_version": "v1",
    "request_id": "task-result-missing",
    "task_id": "task-missing",
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
  const auto body = ParseErrorResponse(response);

  REQUIRE(response.result() == http::status::not_found);
  REQUIRE(body.error_code == zfleet::protocol::ErrorCode::task_not_found);

}

TEST_CASE("task result rejects agent mismatch") {
  namespace fs = std::filesystem;

  const zfleet::test::ScopedTestDir test_root("server");

  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
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

  http::request<http::string_body> request{http::verb::post,
                                           "/v1/tasks/task-1/result", 11};
  request.body() = R"json({
    "protocol_version": "v1",
    "request_id": "task-result-agent-mismatch",
    "task_id": "task-1",
    "agent_id": "agent-2",
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
  const auto body = ParseErrorResponse(response);

  REQUIRE(response.result() == http::status::bad_request);
  REQUIRE(body.error_code == zfleet::protocol::ErrorCode::task_agent_mismatch);

}

TEST_CASE("task result rejects already finished task") {
  namespace fs = std::filesystem;

  const zfleet::test::ScopedTestDir test_root("server");

  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
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
  database.RecordTaskResult(
      zfleet::protocol::TaskResultRequest{
          .protocol_version = "v1",
          .request_id = "seed-terminal",
          .task_id = "task-1",
          .agent_id = "agent-1",
          .task_type = zfleet::protocol::TaskType::collect_basic_inventory,
          .occurred_at = "2026-05-16T10:00:30Z",
          .status = zfleet::protocol::TaskExecutionStatus::succeeded,
          .result = zfleet::protocol::TaskResultData{
              zfleet::protocol::CollectBasicInventoryResult{
                  .hostname = "devbox-01",
                  .os = "linux",
                  .arch = "x86_64",
                  .agent_version = "0.1.0",
              }},
          .error = std::nullopt,
      },
      std::optional<std::string>{R"json({"hostname":"devbox-01","os":"linux","arch":"x86_64","agent_version":"0.1.0"})json"},
      std::nullopt);
  const zfleet::server::HttpHandler handler(&database);

  http::request<http::string_body> request{http::verb::post,
                                           "/v1/tasks/task-1/result", 11};
  request.body() = R"json({
    "protocol_version": "v1",
    "request_id": "task-result-finished",
    "task_id": "task-1",
    "agent_id": "agent-1",
    "task_type": "collect_basic_inventory",
    "occurred_at": "2026-05-16T10:00:40Z",
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
  const auto body = ParseErrorResponse(response);

  REQUIRE(response.result() == http::status::conflict);
  REQUIRE(body.error_code == zfleet::protocol::ErrorCode::task_already_finished);

}

TEST_CASE("task result rejects non-expired terminal update for expired task") {
  namespace fs = std::filesystem;

  const zfleet::test::ScopedTestDir test_root("server");

  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  database.EnqueueTask(zfleet::protocol::Task{
      .protocol_version = "v1",
      .task_id = "task-expired-1",
      .agent_id = "agent-1",
      .task_type = zfleet::protocol::TaskType::collect_basic_inventory,
      .capability_level = zfleet::protocol::CapabilityLevel::readonly,
      .created_at = "2026-05-16T10:00:01Z",
      .expires_at = "2000-05-16T10:05:00Z",
      .input = zfleet::protocol::CollectBasicInventoryInput{},
  });
  const zfleet::server::HttpHandler handler(&database);

  http::request<http::string_body> request{http::verb::post,
                                           "/v1/tasks/task-expired-1/result", 11};
  request.body() = R"json({
    "protocol_version": "v1",
    "request_id": "task-result-expired",
    "task_id": "task-expired-1",
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
  const auto body = ParseErrorResponse(response);

  REQUIRE(response.result() == http::status::conflict);
  REQUIRE(body.error_code == zfleet::protocol::ErrorCode::task_expired);

}
