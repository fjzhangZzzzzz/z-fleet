#include "config.h"
#include "database.h"
#include "http2_connection_registry.h"
#include "http2_control_dispatcher.h"
#include "http2_control_service.h"

#include "test_util.h"

#include "zfleet/protocol/v1/agent_control.pb.h"
#include "zfleet/transport/frame_codec.h"

#include <SQLiteCpp/SQLiteCpp.h>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

namespace {

namespace proto = zfleet::protocol::v1;

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

proto::AgentEvent RegisterEvent(std::string message_id,
                                std::string agent_id,
                                std::string occurred_at) {
  proto::AgentEvent event;
  event.set_protocol_version("v1");
  event.set_message_id(std::move(message_id));
  event.set_agent_id(std::move(agent_id));
  event.set_occurred_at(std::move(occurred_at));
  auto* payload = event.mutable_register_();
  payload->set_agent_version("0.1.0");
  payload->set_hostname("devbox-01");
  payload->set_os("linux");
  payload->set_arch("x86_64");
  return event;
}

proto::AgentEvent HeartbeatEvent(std::string message_id,
                                 std::string agent_id,
                                 std::string occurred_at) {
  proto::AgentEvent event;
  event.set_protocol_version("v1");
  event.set_message_id(std::move(message_id));
  event.set_agent_id(std::move(agent_id));
  event.set_occurred_at(std::move(occurred_at));
  event.mutable_heartbeat()->set_agent_version("0.1.0");
  return event;
}

proto::AgentEvent TaskRunningEvent(std::string message_id,
                                   std::string agent_id,
                                   std::string task_id,
                                   std::string occurred_at) {
  proto::AgentEvent event;
  event.set_protocol_version("v1");
  event.set_message_id(std::move(message_id));
  event.set_agent_id(std::move(agent_id));
  event.set_occurred_at(std::move(occurred_at));
  auto* payload = event.mutable_task_running();
  payload->set_task_id(std::move(task_id));
  payload->set_task_type(proto::TASK_TYPE_COLLECT_BASIC_INVENTORY);
  return event;
}

proto::AgentEvent TaskSucceededEvent(std::string message_id,
                                     std::string agent_id,
                                     std::string task_id,
                                     std::string occurred_at) {
  proto::AgentEvent event;
  event.set_protocol_version("v1");
  event.set_message_id(std::move(message_id));
  event.set_agent_id(std::move(agent_id));
  event.set_occurred_at(std::move(occurred_at));
  auto* payload = event.mutable_task_result();
  payload->set_task_id(std::move(task_id));
  payload->set_task_type(proto::TASK_TYPE_COLLECT_BASIC_INVENTORY);
  payload->set_status(proto::TASK_EXECUTION_STATUS_SUCCEEDED);
  auto* inventory = payload->mutable_collect_basic_inventory();
  inventory->set_hostname("devbox-01");
  inventory->set_os("linux");
  inventory->set_arch("x86_64");
  inventory->set_agent_version("0.1.0");
  return event;
}

void SeedAgent(zfleet::server::ServerDatabase* database,
               std::string agent_id) {
  database->UpsertAgent(zfleet::protocol::RegistrationRequest{
      .protocol_version = "v1",
      .request_id = "seed-agent",
      .agent_id = std::move(agent_id),
      .occurred_at = "2026-05-21T09:00:00Z",
      .agent_version = "0.1.0",
      .hostname = "devbox-01",
      .os = "linux",
      .arch = "x86_64",
  });
}

void SeedTask(zfleet::server::ServerDatabase* database,
              std::string task_id,
              std::string agent_id) {
  database->EnqueueTask(zfleet::protocol::Task{
      .protocol_version = "v1",
      .task_id = std::move(task_id),
      .agent_id = std::move(agent_id),
      .task_type = zfleet::protocol::TaskType::collect_basic_inventory,
      .capability_level = zfleet::protocol::CapabilityLevel::readonly,
      .created_at = "2026-05-21T10:00:00Z",
      .expires_at = "2099-05-21T10:05:00Z",
      .input = zfleet::protocol::CollectBasicInventoryInput{},
  });
}

std::vector<std::uint8_t> EncodeEventFrame(const proto::AgentEvent& event) {
  std::string bytes;
  REQUIRE(event.SerializeToString(&bytes));
  return zfleet::transport::EncodeFrame(std::span<const std::uint8_t>{
      reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()});
}

} // namespace

TEST_CASE("server config loads control listen and database path from toml") {
  const zfleet::test::ScopedTestDir test_root("server");

  const auto config_path = test_root / "server.toml";
  {
    std::ofstream config_stream(config_path);
    REQUIRE(config_stream);
    config_stream << "[server]\n";
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

TEST_CASE("http2 control service registers agent and stores heartbeat") {
  const zfleet::test::ScopedTestDir test_root("server");

  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  const zfleet::server::Http2ControlService service(&database);

  const auto register_result = service.HandleAgentEvent(RegisterEvent(
      "http2-register", "agent-1", "2026-05-21T10:00:00Z"));
  const auto heartbeat_result = service.HandleAgentEvent(HeartbeatEvent(
      "http2-heartbeat", "agent-1", "2026-05-21T10:00:05Z"));

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
  const auto heartbeat_result = service.HandleAgentEvent(HeartbeatEvent(
      "http2-unregistered-heartbeat", "agent-1", "2026-05-21T10:00:05Z"));

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

TEST_CASE("http2 control service stores task running and result events") {
  const zfleet::test::ScopedTestDir test_root("server");

  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  SeedAgent(&database, "agent-1");
  SeedTask(&database, "task-1", "agent-1");
  REQUIRE(database.ClaimNextTaskForAgent("agent-1",
                                         "2026-05-21T10:00:01Z")
              .has_value());
  const zfleet::server::Http2ControlService service(&database);

  const auto running_result = service.HandleAgentEvent(TaskRunningEvent(
      "task-running-1", "agent-1", "task-1", "2026-05-21T10:00:02Z"));
  const auto result_result = service.HandleAgentEvent(TaskSucceededEvent(
      "task-result-1", "agent-1", "task-1", "2026-05-21T10:00:03Z"));

  REQUIRE(running_result.status ==
          zfleet::server::ControlEventStatus::kAccepted);
  REQUIRE(running_result.message == "running");
  REQUIRE(result_result.status == zfleet::server::ControlEventStatus::kAccepted);
  REQUIRE(result_result.message == "accepted");
  REQUIRE(ReadTaskField(database_path, "task-1", "state") == "succeeded");
  REQUIRE(ReadTaskField(database_path, "task-1", "completed_at") ==
          "2026-05-21T10:00:03Z");
  REQUIRE(CountRows(database_path, "task_results") == 1);
  REQUIRE(ReadAuditField(database_path, "task-running-1", "event_type") ==
          "task.running");
  REQUIRE(ReadAuditField(database_path, "task-result-1", "event_type") ==
          "task.succeeded");
}

TEST_CASE("http2 connection registry tracks active heartbeat ownership") {
  zfleet::server::Http2ConnectionRegistry registry;

  registry.OpenConnection("conn-1", "2026-05-21T10:00:00Z");
  registry.BindAgent("conn-1", "agent-1", "2026-05-21T10:00:01Z");
  registry.RecordHeartbeat("conn-1", "agent-1", "2026-05-21T10:00:05Z");

  REQUIRE(registry.ActiveConnectionCount() == 1);
  const auto first_connection = registry.FindByAgent("agent-1");
  REQUIRE(first_connection.has_value());
  REQUIRE(first_connection->connection_id == "conn-1");
  REQUIRE(first_connection->last_heartbeat_at == "2026-05-21T10:00:05Z");
  REQUIRE(registry.IsAgentOnline("agent-1", "2026-05-21T10:00:04Z"));
  REQUIRE_FALSE(registry.IsAgentOnline("agent-1", "2026-05-21T10:00:06Z"));

  registry.OpenConnection("conn-2", "2026-05-21T10:00:10Z");
  registry.BindAgent("conn-2", "agent-1", "2026-05-21T10:00:11Z");

  const auto old_connection = registry.FindByConnection("conn-1");
  const auto current_connection = registry.FindByAgent("agent-1");
  REQUIRE(old_connection.has_value());
  REQUIRE(old_connection->disconnected_at == "2026-05-21T10:00:11Z");
  REQUIRE(current_connection.has_value());
  REQUIRE(current_connection->connection_id == "conn-2");
  REQUIRE(registry.ActiveConnectionCount() == 1);

  registry.CloseConnection("conn-2", "2026-05-21T10:00:20Z");

  REQUIRE_FALSE(registry.FindByAgent("agent-1").has_value());
  REQUIRE(registry.ActiveConnectionCount() == 0);
}

TEST_CASE("http2 control dispatcher decodes framed protobuf event stream") {
  const zfleet::test::ScopedTestDir test_root("server");

  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  zfleet::server::Http2ControlService service(&database);
  zfleet::server::Http2ConnectionRegistry registry;
  registry.OpenConnection("conn-1", "2026-05-21T11:00:00Z");
  zfleet::server::Http2ControlDispatcher dispatcher(
      &service, &registry, "conn-1");

  const auto registration_frame = EncodeEventFrame(RegisterEvent(
      "framed-register", "agent-1", "2026-05-21T11:00:00Z"));
  const auto heartbeat_frame = EncodeEventFrame(HeartbeatEvent(
      "framed-heartbeat", "agent-1", "2026-05-21T11:00:05Z"));

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
  const auto connection = registry.FindByAgent("agent-1");
  REQUIRE(connection.has_value());
  REQUIRE(connection->connection_id == "conn-1");
  REQUIRE(connection->last_heartbeat_at == "2026-05-21T11:00:05Z");
  REQUIRE(registry.IsAgentOnline("agent-1", "2026-05-21T11:00:04Z"));
}
