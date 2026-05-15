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
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <thread>

namespace {

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
