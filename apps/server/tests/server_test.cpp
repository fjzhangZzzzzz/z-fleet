#include "config.h"
#include "database.h"

#include <SQLiteCpp/SQLiteCpp.h>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

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

std::filesystem::path MakeTestRoot() {
  return std::filesystem::temp_directory_path() / "zfleet-server-tests" /
         "config-and-schema";
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
  REQUIRE(database.schema_version() == 1);
  REQUIRE(TableExists(database_path, "agents"));
  REQUIRE(TableExists(database_path, "heartbeats"));
  REQUIRE(TableExists(database_path, "asset_snapshots"));
  REQUIRE(TableExists(database_path, "audit_events"));

  fs::remove_all(test_root);
}
