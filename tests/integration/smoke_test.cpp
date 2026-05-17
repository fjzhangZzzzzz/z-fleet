#include "app.h"
#include "config.h"
#include "database.h"
#include "http_handler.h"
#include "http_server.h"

#include "zfleet/core/log.h"
#include "zfleet/core/time.h"
#include "zfleet/core/version.h"
#include "zfleet/platform/system.h"
#include "zfleet/protocol/message.h"

#include <SQLiteCpp/SQLiteCpp.h>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

namespace {

namespace asio = boost::asio;
namespace http = boost::beast::http;
using tcp = asio::ip::tcp;

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

std::string ReadSingleTextColumn(const std::filesystem::path& database_path,
                                 const std::string& query_text,
                                 const std::string& parameter) {
  SQLite::Database db(database_path.string(), SQLite::OPEN_READONLY);
  SQLite::Statement query(db, query_text);
  query.bind(1, parameter);
  if (!query.executeStep()) {
    return {};
  }
  return query.getColumn(0).getString();
}

std::string ReadSingleTextColumnNoParam(const std::filesystem::path& database_path,
                                        const std::string& query_text) {
  SQLite::Database db(database_path.string(), SQLite::OPEN_READONLY);
  SQLite::Statement query(db, query_text);
  if (!query.executeStep()) {
    return {};
  }
  return query.getColumn(0).getString();
}

http::response<http::string_body> PostJson(std::uint16_t port,
                                           std::string_view target,
                                           std::string body) {
  asio::io_context io_context;
  tcp::resolver resolver(io_context);
  tcp::socket socket(io_context);
  const auto endpoints = resolver.resolve("127.0.0.1", std::to_string(port));
  asio::connect(socket, endpoints);

  http::request<http::string_body> request{http::verb::post,
                                           std::string(target), 11};
  request.set(http::field::host, "127.0.0.1");
  request.set(http::field::content_type, "application/json");
  request.body() = std::move(body);
  request.prepare_payload();
  http::write(socket, request);

  boost::beast::flat_buffer buffer;
  http::response<http::string_body> response;
  http::read(socket, buffer, response);
  return response;
}

struct RunningServer {
  zfleet::server::ServerDatabase database;
  zfleet::server::HttpHandler handler;
  zfleet::server::HttpServer server;
  std::thread thread;

  explicit RunningServer(const std::filesystem::path& database_path)
      : database(database_path),
        handler(&database),
        server("127.0.0.1:0", &handler) {
    database.Initialize();
    thread = std::thread([this]() { server.Run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  ~RunningServer() {
    server.Stop();
    if (thread.joinable()) {
      thread.join();
    }
  }
};

std::filesystem::path MakeTestRoot() {
  return std::filesystem::temp_directory_path() / "zfleet-integration-tests" /
         "agent-server-run-once";
}

} // namespace

TEST_CASE("integration scaffold links core platform and protocol modules") {
  REQUIRE_FALSE(zfleet::core::project_name().empty());
  REQUIRE_FALSE(zfleet::core::version().empty());
  REQUIRE_FALSE(zfleet::platform::os_name().empty());
  REQUIRE_FALSE(zfleet::protocol::protocol_version().empty());
  REQUIRE_FALSE(zfleet::core::NowUtcRfc3339().empty());
}

TEST_CASE("server startup initializes schema for integration flow") {
  namespace fs = std::filesystem;

  const auto test_root = MakeTestRoot();
  fs::remove_all(test_root);
  fs::create_directories(test_root);
  const auto database_path = test_root / "zfleet.db";

  {
    RunningServer server(database_path);
    REQUIRE(TableExists(database_path, "agents"));
    REQUIRE(TableExists(database_path, "heartbeats"));
    REQUIRE(TableExists(database_path, "asset_snapshots"));
  }

  fs::remove_all(test_root);
}

TEST_CASE("agent run once registers heartbeats and uploads assets end to end") {
  namespace fs = std::filesystem;

  const auto test_root = MakeTestRoot();
  fs::remove_all(test_root);
  fs::create_directories(test_root);
  const auto database_path = test_root / "zfleet.db";
  const auto agent_data_dir = test_root / "agent-data";

  {
    RunningServer server(database_path);
    const zfleet::agent::AgentConfig config{
        .server_url =
            "http://127.0.0.1:" + std::to_string(server.server.port()),
        .data_dir = agent_data_dir,
        .state_file = "state.toml",
        .log =
            {
                .level = zfleet::core::log::Level::kError,
                .enable_console = false,
                .file_path = test_root / "agent.log",
            },
    };

    const auto result = zfleet::agent::RunOnce(config);

    REQUIRE_FALSE(result.agent_id.empty());
    REQUIRE(fs::exists(agent_data_dir / "state.toml"));
    REQUIRE(CountRows(database_path, "agents") == 1);
    REQUIRE(CountRows(database_path, "heartbeats") == 1);
    REQUIRE(CountRows(database_path, "asset_snapshots") == 1);
    REQUIRE(ReadSingleTextColumn(database_path,
                                 "select agent_id from agents where agent_id = ?",
                                 result.agent_id) == result.agent_id);
    REQUIRE(ReadSingleTextColumn(
                database_path,
                "select agent_id from heartbeats where agent_id = ?",
                result.agent_id) == result.agent_id);
    REQUIRE(ReadSingleTextColumn(
                database_path,
                "select agent_id from asset_snapshots where agent_id = ?",
                result.agent_id) == result.agent_id);
  }

  fs::remove_all(test_root);
}

TEST_CASE("agent run once polls task and uploads result end to end") {
  namespace fs = std::filesystem;

  const auto test_root = MakeTestRoot();
  fs::remove_all(test_root);
  fs::create_directories(test_root);
  const auto database_path = test_root / "zfleet-tasks.db";
  const auto agent_data_dir = test_root / "agent-task-data";

  {
    RunningServer server(database_path);
    const auto seeded_agent_id = "agent-task-1";
    server.database.UpsertAgent(zfleet::protocol::RegistrationRequest{
        .protocol_version = "v1",
        .request_id = "seed-agent",
        .agent_id = seeded_agent_id,
        .occurred_at = "2026-05-17T10:00:00Z",
        .agent_version = "0.1.0",
        .hostname = "devbox-01",
        .os = "linux",
        .arch = "x86_64",
    });
    const auto create_response = PostJson(
        server.server.port(), "/v1/tasks",
        R"json({
          "protocol_version": "v1",
          "request_id": "task-create-integration-1",
          "occurred_at": "2026-05-17T10:00:01Z",
          "task": {
            "protocol_version": "v1",
            "task_id": "task-integration-1",
            "agent_id": "agent-task-1",
            "task_type": "collect_basic_inventory",
            "capability_level": "readonly",
            "created_at": "2026-05-17T10:00:01Z",
            "expires_at": "2099-05-17T10:05:00Z",
            "input": {}
          }
        })json");
    REQUIRE(create_response.result() == http::status::ok);

    fs::create_directories(agent_data_dir);
    {
      std::ofstream state_stream(agent_data_dir / "state.toml");
      REQUIRE(state_stream);
      state_stream << "[state]\n";
      state_stream << "agent_id = \"" << seeded_agent_id << "\"\n";
    }

    const zfleet::agent::AgentConfig config{
        .server_url =
            "http://127.0.0.1:" + std::to_string(server.server.port()),
        .data_dir = agent_data_dir,
        .state_file = "state.toml",
        .log =
            {
                .level = zfleet::core::log::Level::kError,
                .enable_console = false,
                .file_path = test_root / "agent-task.log",
            },
    };

    const auto result = zfleet::agent::RunOnce(config);

    REQUIRE(result.agent_id == seeded_agent_id);
    REQUIRE(result.completed_task_id == std::optional<std::string>{"task-integration-1"});
    REQUIRE(CountRows(database_path, "task_results") == 1);
    REQUIRE(ReadSingleTextColumn(
                database_path,
                "select state from tasks where task_id = ?",
                "task-integration-1") == "succeeded");
    REQUIRE(ReadSingleTextColumn(
                database_path,
                "select task_type from task_results where task_id = ?",
                "task-integration-1") == "collect_basic_inventory");
    REQUIRE(CountRows(database_path, "task_results") == 1);
    REQUIRE(ReadSingleTextColumnNoParam(
                database_path,
                "select count(*) from audit_events where event_type = 'task.running'") ==
            "1");
    REQUIRE(ReadSingleTextColumnNoParam(
                database_path,
                "select event_type from audit_events where event_type like 'task.%' order by rowid desc limit 1") ==
            "task.succeeded");
  }

  fs::remove_all(test_root);
}
