#include "config.h"

#include "test_util.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

namespace {

TEST_CASE("server config loads control listen and database path from toml") {
  const zfleet::test::ScopedTestDir test_root("server");

  const auto config_path = test_root / "server.toml";
  {
    std::ofstream config_stream(config_path);
    REQUIRE(config_stream);
    config_stream << "[server]\n";
    config_stream << "control_listen = \"127.0.0.1:18081\"\n";
    config_stream << "admin_listen = \"127.0.0.1:18080\"\n";
    config_stream << "admin_public_url = \"http://server.example:18080\"\n";
    config_stream << "control_public_url = \"http://server.example:18081\"\n";
    config_stream << "database_path = \"data/server.db\"\n";
    config_stream << "package_repository = \"data/packages\"\n";
    config_stream << "web_static_dir = \"share/web\"\n";
    config_stream << "allow_high_risk_write = true\n";
    config_stream << "\n[log]\n";
    config_stream << "level = \"debug\"\n";
    config_stream << "file = \"logs/server.log\"\n";
    config_stream << "enable_console = false\n";
  }

  auto config = zfleet::server::LoadConfig(config_path);
  config.install_dir = test_root.path();
  zfleet::server::ResolveConfigPaths(&config);

  REQUIRE(config.install_dir == test_root.path());
  REQUIRE(config.control_listen == "127.0.0.1:18081");
  REQUIRE(config.admin_listen == "127.0.0.1:18080");
  REQUIRE(config.admin_public_url == "http://server.example:18080");
  REQUIRE(config.control_public_url == "http://server.example:18081");
  REQUIRE(config.database_path == test_root / "data" / "server.db");
  REQUIRE(config.package_repository == test_root / "data" / "packages");
  REQUIRE(config.web_static_dir == test_root / "share" / "web");
  REQUIRE(config.allow_high_risk_write);
  REQUIRE(config.log.level == zfleet::core::log::Level::kDebug);
  REQUIRE(config.log.file_path == test_root / "logs" / "server.log");
  REQUIRE_FALSE(config.log.enable_console);
}

TEST_CASE(
    "server config persists defaults and CLI overrides without install dir") {
  const zfleet::test::ScopedTestDir test_root("server");
  const auto config_path = zfleet::server::DefaultConfigPath(
      std::optional<std::filesystem::path>{test_root.path()});

  zfleet::server::ServerConfig config;
  config.install_dir = test_root.path();
  config.control_listen = "127.0.0.1:18081";
  config.admin_listen = "127.0.0.1:18080";
  config.admin_public_url = "http://server.example:18080";
  config.control_public_url = "http://server.example:18081";
  config.database_path = "data/custom.db";
  config.package_repository = "data/packages";
  config.web_static_dir = "share/web";
  config.allow_high_risk_write = true;
  config.log.level = zfleet::core::log::Level::kError;

  zfleet::server::SaveConfig(config, config_path);

  REQUIRE(config_path == test_root / "etc" / "server.toml");
  const auto saved = zfleet::test::ReadTextFile(config_path);
  REQUIRE(saved.find("install_dir") == std::string::npos);
  REQUIRE(saved.find("control_listen") != std::string::npos);
  REQUIRE(saved.find("127.0.0.1:18081") != std::string::npos);
  REQUIRE(saved.find("database_path") != std::string::npos);
  REQUIRE(saved.find("data/custom.db") != std::string::npos);
  REQUIRE(saved.find("admin_listen") != std::string::npos);
  REQUIRE(saved.find("admin_public_url") != std::string::npos);
  REQUIRE(saved.find("control_public_url") != std::string::npos);
  REQUIRE(saved.find("package_repository") != std::string::npos);
  REQUIRE(saved.find("web_static_dir") != std::string::npos);
  REQUIRE(saved.find("allow_high_risk_write") != std::string::npos);

  auto loaded = zfleet::server::LoadConfig(config_path);
  loaded.install_dir = test_root.path();
  zfleet::server::ResolveConfigPaths(&loaded);

  REQUIRE(loaded.install_dir == test_root.path());
  REQUIRE(loaded.control_listen == "127.0.0.1:18081");
  REQUIRE(loaded.admin_listen == "127.0.0.1:18080");
  REQUIRE(loaded.admin_public_url == "http://server.example:18080");
  REQUIRE(loaded.control_public_url == "http://server.example:18081");
  REQUIRE(loaded.database_path == test_root / "data" / "custom.db");
  REQUIRE(loaded.package_repository == test_root / "data" / "packages");
  REQUIRE(loaded.web_static_dir == test_root / "share" / "web");
  REQUIRE(loaded.allow_high_risk_write);
  REQUIRE(loaded.log.level == zfleet::core::log::Level::kError);
}

TEST_CASE(
    "server config leaves Web assets on active release unless overridden") {
  const zfleet::server::ServerConfig config;
  REQUIRE(config.web_static_dir.empty());
}

}  // namespace
