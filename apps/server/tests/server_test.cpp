#include "config.h"
#include "database.h"
#include "control_connection_registry.h"
#include "control_dispatcher.h"
#include "control_server.h"
#include "control_service.h"
#include "admin_http_server.h"
#include "package_repository.h"

#include "test_util.h"

#include "zfleet/protocol/v1/agent_control.pb.h"
#include "zfleet/crypto/sha256.h"
#include "zfleet/package/archive.h"
#include "zfleet/package/manifest.h"
#include "zfleet/transport/frame_codec.h"

#include <SQLiteCpp/SQLiteCpp.h>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/asio/write.hpp>
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
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

std::string ReadAgentField(const std::filesystem::path& database_path,
                           const std::string& agent_id,
                           const std::string& column_name) {
  SQLite::Database db(database_path.string(), SQLite::OPEN_READONLY);
  SQLite::Statement query(
      db, "select " + column_name + " from agents where agent_id = ?");
  query.bind(1, agent_id);
  if (!query.executeStep() || query.getColumn(0).isNull()) {
    return {};
  }
  return query.getColumn(0).getString();
}

std::string ReadAgentLastSeen(const std::filesystem::path& database_path,
                              const std::string& agent_id) {
  return ReadAgentField(database_path, agent_id, "last_seen_at");
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

std::string ReadTaskResultField(const std::filesystem::path& database_path,
                                const std::string& task_id,
                                const std::string& column_name) {
  SQLite::Database db(database_path.string(), SQLite::OPEN_READONLY);
  SQLite::Statement query(
      db, "select " + column_name + " from task_results where task_id = ?");
  query.bind(1, task_id);
  if (!query.executeStep() || query.getColumn(0).isNull()) {
    return {};
  }
  return query.getColumn(0).getString();
}

std::string ReadTaskResultBlob(const std::filesystem::path& database_path,
                               const std::string& task_id,
                               const std::string& column_name) {
  SQLite::Database db(database_path.string(), SQLite::OPEN_READONLY);
  SQLite::Statement query(
      db, "select " + column_name + " from task_results where task_id = ?");
  query.bind(1, task_id);
  if (!query.executeStep() || query.getColumn(0).isNull()) {
    return {};
  }
  const auto column = query.getColumn(0);
  return std::string(static_cast<const char*>(column.getBlob()),
                     static_cast<std::size_t>(column.getBytes()));
}

std::string ReadAssetSnapshotField(
    const std::filesystem::path& database_path,
    const std::string& agent_id,
    const std::string& column_name) {
  SQLite::Database db(database_path.string(), SQLite::OPEN_READONLY);
  SQLite::Statement query(
      db, "select " + column_name +
              " from asset_snapshots where agent_id = ? order by snapshot_id desc limit 1");
  query.bind(1, agent_id);
  if (!query.executeStep() || query.getColumn(0).isNull()) {
    return {};
  }
  return query.getColumn(0).getString();
}

std::string ReadAssetSnapshotBlob(
    const std::filesystem::path& database_path,
    const std::string& agent_id) {
  SQLite::Database db(database_path.string(), SQLite::OPEN_READONLY);
  SQLite::Statement query(
      db,
      "select event_blob from asset_snapshots where agent_id = ? order by snapshot_id desc limit 1");
  query.bind(1, agent_id);
  if (!query.executeStep()) {
    return {};
  }
  const auto column = query.getColumn(0);
  return std::string(static_cast<const char*>(column.getBlob()),
                     static_cast<std::size_t>(column.getBytes()));
}


proto::AgentEvent RegisterEvent(std::string message_id,
                                std::string agent_id,
                                std::string occurred_at,
                                std::string registration_token = {},
                                std::string agent_version = "0.1.0") {
  proto::AgentEvent event;
  event.set_protocol_version("v1");
  event.set_message_id(std::move(message_id));
  event.set_agent_id(std::move(agent_id));
  event.set_occurred_at(std::move(occurred_at));
  auto* payload = event.mutable_register_();
  payload->set_agent_version(std::move(agent_version));
  payload->set_hostname("devbox-01");
  payload->set_os("linux");
  payload->set_arch("x86_64");
  payload->set_registration_token(std::move(registration_token));
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

proto::AgentEvent AssetSnapshotEvent(std::string message_id,
                                     std::string agent_id,
                                     std::string occurred_at) {
  proto::AgentEvent event;
  event.set_protocol_version("v1");
  event.set_message_id(std::move(message_id));
  event.set_agent_id(std::move(agent_id));
  event.set_occurred_at(std::move(occurred_at));
  auto* payload = event.mutable_asset_snapshot();
  payload->set_hostname("devbox-01");
  payload->set_os("linux");
  payload->set_os_version("6.8");
  payload->set_arch("x86_64");
  payload->set_agent_version("0.1.0");
  payload->add_applications("cmake");
  payload->add_services("zfleet-agent");
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

proto::AgentEvent TaskFailedEvent(std::string message_id,
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
  payload->set_status(proto::TASK_EXECUTION_STATUS_FAILED);
  auto* error = payload->mutable_error();
  error->set_code(proto::ERROR_CODE_INTERNAL_ERROR);
  error->set_message("inventory failed");
  error->set_retryable(true);
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
  database->UpsertAgent(zfleet::protocol::AgentRegistration{
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

struct HttpResponse {
  int status = 0;
  std::string headers;
  std::string body;
};

HttpResponse ReadHttpResponse(boost::asio::ip::tcp::socket* socket) {
  boost::asio::streambuf response;
  boost::system::error_code ec;
  boost::asio::read(*socket, response, ec);
  REQUIRE(ec == boost::asio::error::eof);

  std::istream stream(&response);
  std::string text((std::istreambuf_iterator<char>(stream)),
                   std::istreambuf_iterator<char>());
  const auto header_end = text.find("\r\n\r\n");
  REQUIRE(header_end != std::string::npos);
  const auto status_end = text.find("\r\n");
  REQUIRE(status_end != std::string::npos);
  const auto status_line = text.substr(0, status_end);
  HttpResponse parsed;
  parsed.status = std::stoi(status_line.substr(9, 3));
  parsed.headers = text.substr(0, header_end);
  parsed.body = text.substr(header_end + 4);
  return parsed;
}

HttpResponse SendHttpRequest(std::uint16_t port,
                             const std::string& request) {
  boost::asio::io_context io_context;
  boost::asio::ip::tcp::socket socket(io_context);
  socket.connect({boost::asio::ip::make_address("127.0.0.1"), port});
  boost::asio::write(socket, boost::asio::buffer(request));
  return ReadHttpResponse(&socket);
}

std::string ReadBinaryFile(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  REQUIRE(stream);
  std::ostringstream contents;
  contents << stream.rdbuf();
  return contents.str();
}

std::filesystem::path FindRepoWebRoot() {
  auto current = std::filesystem::current_path();
  for (int depth = 0; depth < 8; ++depth) {
    const auto candidate = current / "apps" / "server" / "web";
    if (std::filesystem::exists(candidate / "install.html") &&
        std::filesystem::exists(candidate / "agents.html") &&
        std::filesystem::exists(candidate / "assets" / "admin.js")) {
      return candidate;
    }
    if (current == current.root_path()) {
      break;
    }
    current = current.parent_path();
  }
  throw std::runtime_error("failed to locate apps/server/web from test cwd");
}

std::filesystem::path CreatePackageArchive(
    const std::filesystem::path& root, const std::string& component,
    const std::string& version, const std::string& build_type,
    const std::string& min_installer_version = "0.1.0") {
  const auto package_dir = root / ("package-" + component);
  const auto binary = "zfleet_" + component;
  const auto contents = component + "-binary";
  const auto payload_file = package_dir / "payload" / "bin" / binary;
  zfleet::test::WriteTextFile(payload_file, contents);

  const auto manifest = zfleet::package::SerializeManifestJson(
      zfleet::package::Manifest{
          .schema_version = 1,
          .component = component,
          .version = version,
          .platform = "linux",
          .arch = "x86_64",
          .build_type = build_type,
          .min_installer_version = min_installer_version,
          .files = {zfleet::package::ManifestFile{
              .source = "payload/bin/" + binary,
              .target = "bin/" + binary,
              .size = static_cast<std::uint64_t>(contents.size()),
              .sha256 = zfleet::crypto::Sha256BytesHex(contents),
              .executable = true,
          }},
      });
  zfleet::test::WriteTextFile(package_dir / "META" / "manifest.json",
                              manifest);

  const auto archive_path = root / (component + ".zip");
  zfleet::package::CreateArchive(zfleet::package::CreateArchiveOptions{
      .package_dir = package_dir,
      .archive_path = archive_path,
      .force = true,
  });
  return archive_path;
}

void WriteWebStaticFiles(const std::filesystem::path& root) {
  zfleet::test::WriteTextFile(root / "index.html", "<title>z-fleet</title>");
  zfleet::test::WriteTextFile(root / "install.html",
                              "<title>z-fleet install</title>");
  zfleet::test::WriteTextFile(root / "agents.html",
                              "<title>z-fleet agents</title>");
  zfleet::test::WriteTextFile(root / "admin" / "packages.html",
                              "<title>z-fleet packages</title>");
  zfleet::test::WriteTextFile(root / "assets" / "admin.css",
                              "body { color: #102c32; }");
  zfleet::test::WriteTextFile(root / "assets" / "admin.js",
                              "console.log('z-fleet');");
  zfleet::test::WriteTextFile(root / "scripts" / "install" / "linux.sh",
                              "#!/usr/bin/env bash\nsha256sum -c\n");
  zfleet::test::WriteTextFile(root / "scripts" / "install" / "windows.ps1",
                              "Get-FileHash\n");
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
    config_stream << "admin_listen = \"127.0.0.1:18080\"\n";
    config_stream << "admin_public_url = \"http://server.example:18080\"\n";
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
  REQUIRE(config.database_path == test_root / "data" / "server.db");
  REQUIRE(config.package_repository == test_root / "data" / "packages");
  REQUIRE(config.web_static_dir == test_root / "share" / "web");
  REQUIRE(config.allow_high_risk_write);
  REQUIRE(config.log.level == zfleet::core::log::Level::kDebug);
  REQUIRE(config.log.file_path == test_root / "logs" / "server.log");
  REQUIRE_FALSE(config.log.enable_console);
}

TEST_CASE("server config persists defaults and CLI overrides without install dir") {
  const zfleet::test::ScopedTestDir test_root("server");
  const auto config_path = zfleet::server::DefaultConfigPath(
      std::optional<std::filesystem::path>{test_root.path()});

  zfleet::server::ServerConfig config;
  config.install_dir = test_root.path();
  config.control_listen = "127.0.0.1:18081";
  config.admin_listen = "127.0.0.1:18080";
  config.admin_public_url = "http://server.example:18080";
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
  REQUIRE(loaded.database_path == test_root / "data" / "custom.db");
  REQUIRE(loaded.package_repository == test_root / "data" / "packages");
  REQUIRE(loaded.web_static_dir == test_root / "share" / "web");
  REQUIRE(loaded.allow_high_risk_write);
  REQUIRE(loaded.log.level == zfleet::core::log::Level::kError);
}

TEST_CASE("server config leaves Web assets on active release unless overridden") {
  const zfleet::server::ServerConfig config;
  REQUIRE(config.web_static_dir.empty());
}

TEST_CASE("server database initializes schema and version") {
  namespace fs = std::filesystem;

  const zfleet::test::ScopedTestDir test_root("server");

  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();

  REQUIRE(fs::exists(database_path));
  REQUIRE(database.schema_version() == 10);
  REQUIRE(TableExists(database_path, "agents"));
  REQUIRE_FALSE(TableExists(database_path, "heartbeats"));
  REQUIRE(TableExists(database_path, "asset_snapshots"));
  REQUIRE(TableExists(database_path, "audit_events"));
  REQUIRE(TableExists(database_path, "tasks"));
  REQUIRE(TableExists(database_path, "task_results"));
  REQUIRE(TableExists(database_path, "agent_packages"));
  REQUIRE(TableExists(database_path, "package_publications"));
  REQUIRE(TableExists(database_path, "registration_tokens"));
}

TEST_CASE("server database exposes web admin read models") {
  const zfleet::test::ScopedTestDir test_root("server");

  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  SeedAgent(&database, "agent-web-1");
  database.RecordAssetSnapshot(
      zfleet::protocol::AssetSnapshot{
          .protocol_version = "v1",
          .request_id = "asset-web-1",
          .agent_id = "agent-web-1",
          .occurred_at = "2026-05-24T10:00:00Z",
          .hostname = "devbox-web",
          .os = "linux",
          .os_version = "6.8",
          .arch = "x86_64",
          .agent_version = "0.1.0",
          .applications = {"cmake", "ninja"},
          .services = {"zfleet-agent"},
      },
      AssetSnapshotEvent("asset-web-1", "agent-web-1",
                         "2026-05-24T10:00:00Z"));

  const auto agents = database.ListAgents();
  REQUIRE(agents.size() == 1);
  REQUIRE(agents.front().agent_id == "agent-web-1");
  REQUIRE(agents.front().status == "online");

  const auto latest = database.FindLatestAssetSnapshot("agent-web-1");
  REQUIRE(latest.has_value());
  REQUIRE(latest->hostname == "devbox-web");
  REQUIRE(latest->os_version == "6.8");
  REQUIRE((latest->applications == std::vector<std::string>{"cmake", "ninja"}));
  REQUIRE((latest->services == std::vector<std::string>{"zfleet-agent"}));
}

TEST_CASE("server database stores package channel and registration tokens") {
  const zfleet::test::ScopedTestDir test_root("server");

  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();

  database.UpsertAgentPackage(zfleet::server::AgentPackageRecord{
      .package_id = "pkg-1",
      .component = "agent",
      .version = "0.1.0",
      .platform = "linux",
      .arch = "x86_64",
      .build_type = "release",
      .filename = "zfleet_agent-v0.1.0-linux-x86_64-release.zip",
      .storage_path = test_root / "agent.zip",
      .size_bytes = 10,
      .sha256 = std::string(64, 'a'),
      .manifest_json = "{}",
      .status = "validated",
      .uploaded_at = "2026-05-24T10:00:00Z",
      .validated_at = "2026-05-24T10:00:00Z",
  });
  database.PublishAgentPackage("pkg-1", "agent", "stable", "linux", "x86_64",
                               "release",
                               std::optional<std::string>{"admin"},
                               "2026-05-24T10:01:00Z");

  const auto package =
      database.FindDefaultPublishedAgentPackage("agent", "stable", "linux",
                                                "x86_64", "release");
  REQUIRE(package.has_value());
  REQUIRE(package->package_id == "pkg-1");
  REQUIRE(package->status == "published");
  const auto stored_package = database.FindAgentPackage("pkg-1");
  REQUIRE(stored_package.has_value());
  REQUIRE(stored_package->published_channels == std::vector<std::string>{"stable"});

  database.RetireAgentPackage("pkg-1", "2026-05-24T10:02:00Z");
  const auto retired_package = database.FindAgentPackage("pkg-1");
  REQUIRE(retired_package.has_value());
  REQUIRE(retired_package->status == "retired");
  REQUIRE(retired_package->retired_at == "2026-05-24T10:02:00Z");
  REQUIRE(retired_package->published_channels.empty());

  database.CreateRegistrationToken(zfleet::server::RegistrationTokenRecord{
      .token_id = "token-1",
      .token_hash = std::string(64, 'b'),
      .purpose = "agent_register",
      .channel = std::optional<std::string>{"stable"},
      .platform = std::optional<std::string>{"linux"},
      .arch = std::optional<std::string>{"x86_64"},
      .max_uses = 1,
      .use_count = 0,
      .status = "active",
      .created_at = "2026-05-24T10:02:00Z",
      .expires_at = "2026-05-25T10:02:00Z",
  });
  const auto tokens = database.ListRegistrationTokens();
  REQUIRE(tokens.size() == 1);
  REQUIRE(tokens.front().token_id == "token-1");
  REQUIRE(tokens.front().token_hash == std::string(64, 'b'));
  REQUIRE(tokens.front().status == "active");

  REQUIRE(database.RevokeRegistrationToken("token-1",
                                           "2026-05-24T10:03:00Z"));
  const auto revoked_tokens = database.ListRegistrationTokens();
  REQUIRE(revoked_tokens.front().status == "revoked");
  REQUIRE(revoked_tokens.front().revoked_at ==
          std::optional<std::string>{"2026-05-24T10:03:00Z"});
}

TEST_CASE("server package validation checks archive metadata and filename") {
  const zfleet::test::ScopedTestDir test_root("server");
  const auto package_dir = test_root / "package";
  zfleet::test::WriteTextFile(package_dir / "payload" / "bin" / "zfleet_agent",
                              "agent-binary");
  const auto manifest = zfleet::package::SerializeManifestJson(
      zfleet::package::Manifest{
          .schema_version = 1,
          .component = "agent",
          .version = "0.1.0",
          .platform = "linux",
          .arch = "x86_64",
          .build_type = "release",
          .min_installer_version = "0.1.0",
          .files = {zfleet::package::ManifestFile{
              .source = "payload/bin/zfleet_agent",
              .target = "bin/zfleet_agent",
              .size = 12,
              .sha256 = zfleet::crypto::Sha256BytesHex("agent-binary"),
              .executable = true,
          }},
      });
  zfleet::test::WriteTextFile(package_dir / "META" / "manifest.json",
                              manifest);
  const auto archive_path =
      test_root / "zfleet_agent-v0.1.0-linux-x86_64-release.zip";
  zfleet::package::CreateArchive(zfleet::package::CreateArchiveOptions{
      .package_dir = package_dir,
      .archive_path = archive_path,
      .force = true,
  });

  const auto metadata = zfleet::server::ValidateAgentPackageUpload(
      archive_path, "zfleet_agent-v0.1.0-linux-x86_64-release.zip");
  REQUIRE(metadata.component == "agent");
  REQUIRE(metadata.version == "0.1.0");
  REQUIRE(metadata.platform == "linux");
  REQUIRE(metadata.arch == "x86_64");
  REQUIRE(metadata.build_type == "release");
}

TEST_CASE("admin http server serves static UI and agent api") {
  const zfleet::test::ScopedTestDir test_root("server");

  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  SeedAgent(&database, "agent-http-1");
  const auto web_root = test_root / "share" / "web";
  WriteWebStaticFiles(web_root);

  zfleet::server::AdminHttpServer server(
      "127.0.0.1:0",
      &database,
      test_root / "packages",
      web_root,
      zfleet::server::AdminHttpServerOptions{
          .control_url = "http://127.0.0.1:8081",
      });
  server.Start();

  const auto page = SendHttpRequest(
      server.port(),
      "GET /install HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
  REQUIRE(page.status == 200);
  REQUIRE(page.headers.find("text/html") != std::string::npos);
  REQUIRE(page.body.find("z-fleet") != std::string::npos);

  const auto stylesheet = SendHttpRequest(
      server.port(),
      "GET /assets/admin.css HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
  REQUIRE(stylesheet.status == 200);
  REQUIRE(stylesheet.headers.find("text/css") != std::string::npos);
  REQUIRE(stylesheet.body.find("body { color: #102c32; }") !=
          std::string::npos);

  const auto traversal = SendHttpRequest(
      server.port(),
      "GET /assets/../server.toml HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
  REQUIRE(traversal.status == 404);

  const auto agents = SendHttpRequest(
      server.port(),
      "GET /api/v1/agents HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
  REQUIRE(agents.status == 200);
  REQUIRE(agents.body.find("agent-http-1") != std::string::npos);
  REQUIRE(agents.body.find("\"latest_asset\":null") != std::string::npos);

  const auto agent_detail = SendHttpRequest(
      server.port(),
      "GET /api/v1/agents/agent-http-1 HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
  REQUIRE(agent_detail.status == 200);
  REQUIRE(agent_detail.body.find("\"agent_id\":\"agent-http-1\"") !=
          std::string::npos);
  REQUIRE(agent_detail.body.find("\"latest_asset\":null") !=
          std::string::npos);

  const auto asset_list = SendHttpRequest(
      server.port(),
      "GET /api/v1/agents/agent-http-1/assets HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
  REQUIRE(asset_list.status == 200);
  REQUIRE(asset_list.body.find("\"assets\"") != std::string::npos);

  const auto missing_latest_asset = SendHttpRequest(
      server.port(),
      "GET /api/v1/agents/agent-http-1/assets/latest HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
  REQUIRE(missing_latest_asset.status == 404);
  REQUIRE(missing_latest_asset.body.find("asset_snapshot_not_found") !=
          std::string::npos);

  const auto forbidden_upgrade = SendHttpRequest(
      server.port(),
      "POST /api/v1/agents/agent-http-1/upgrade HTTP/1.1\r\n"
      "Host: 127.0.0.1\r\nContent-Length: 0\r\n\r\n");
  REQUIRE(forbidden_upgrade.status == 403);
  REQUIRE(forbidden_upgrade.body.find("capability_not_allowed") !=
          std::string::npos);
  const auto forbidden_rollback = SendHttpRequest(
      server.port(),
      "POST /api/v1/agents/agent-http-1/rollback HTTP/1.1\r\n"
      "Host: 127.0.0.1\r\nContent-Length: 0\r\n\r\n");
  REQUIRE(forbidden_rollback.status == 403);
  REQUIRE(forbidden_rollback.body.find("capability_not_allowed") !=
          std::string::npos);

  const auto linux_script = SendHttpRequest(
      server.port(),
      "GET /api/v1/install/script?platform=linux HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
  REQUIRE(linux_script.status == 200);
  REQUIRE(linux_script.body.find("sha256sum -c") != std::string::npos);

  const auto windows_script = SendHttpRequest(
      server.port(),
      "GET /api/v1/install/script?platform=windows HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
  REQUIRE(windows_script.status == 200);
  REQUIRE(windows_script.body.find("Get-FileHash") != std::string::npos);

  const auto install_commands = SendHttpRequest(
      server.port(),
      "GET /api/v1/install/commands?server_url=http%3A%2F%2F127.0.0.1%3A8080&token=abc&channel=stable&platform=linux HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
  REQUIRE(install_commands.status == 200);
  REQUIRE(install_commands.body.find("\"command\"") != std::string::npos);
  REQUIRE(install_commands.body.find("\"platform\":\"linux\"") !=
          std::string::npos);
  REQUIRE(install_commands.body.find("/api/v1/install/script?platform=linux") !=
          std::string::npos);
  REQUIRE(install_commands.body.find("--control-url") != std::string::npos);

  const auto install_commands_windows = SendHttpRequest(
      server.port(),
      "GET /api/v1/install/commands?server_url=http%3A%2F%2F127.0.0.1%3A8080&token=abc&channel=stable&platform=windows HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
  REQUIRE(install_commands_windows.status == 200);
  REQUIRE(install_commands_windows.body.find("\"platform\":\"windows\"") !=
          std::string::npos);
  REQUIRE(install_commands_windows.body.find("/api/v1/install/script?platform=windows") !=
          std::string::npos);

  server.Stop();
}

TEST_CASE("admin http server creates scoped registration tokens") {
  const zfleet::test::ScopedTestDir test_root("server");

  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  const auto web_root = test_root / "share" / "web";
  WriteWebStaticFiles(web_root);

  zfleet::server::AdminHttpServer server(
      "127.0.0.1:0",
      &database,
      test_root / "packages",
      web_root,
      zfleet::server::AdminHttpServerOptions{
          .control_url = "http://127.0.0.1:8081",
      });
  server.Start();

  const std::string token_body =
      "{\"purpose\":\"agent_register\",\"expires_at\":\"2026-05-25T10:00:00Z\","
      "\"channel\":\"stable\",\"platform\":\"linux\",\"arch\":\"x86_64\","
      "\"max_uses\":1}";
  const auto response = SendHttpRequest(
      server.port(),
      "POST /api/v1/install/tokens HTTP/1.1\r\nHost: 127.0.0.1\r\n"
      "Content-Type: application/json\r\nContent-Length: " +
      std::to_string(token_body.size()) + "\r\n\r\n" + token_body);
  REQUIRE(response.status == 201);
  REQUIRE(response.body.find("\"token\"") != std::string::npos);
  REQUIRE(response.body.find("\"status\":\"active\"") != std::string::npos);
  REQUIRE(response.body.find("\"purpose\":\"agent_register\"") !=
          std::string::npos);
  REQUIRE(response.body.find("\"max_uses\":1") != std::string::npos);
}

TEST_CASE("admin http server rejects invalid token requests") {
  const zfleet::test::ScopedTestDir test_root("server");

  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  const auto web_root = test_root / "share" / "web";
  WriteWebStaticFiles(web_root);

  zfleet::server::AdminHttpServer server(
      "127.0.0.1:0",
      &database,
      test_root / "packages",
      web_root,
      zfleet::server::AdminHttpServerOptions{
          .control_url = "http://127.0.0.1:8081",
      });
  server.Start();

  const std::string invalid_body =
      "{\"purpose\":\"agent_register\",\"max_uses\":0}";
  const auto response = SendHttpRequest(
      server.port(),
      "POST /api/v1/install/tokens HTTP/1.1\r\nHost: 127.0.0.1\r\n"
      "Content-Type: application/json\r\nContent-Length: " +
      std::to_string(invalid_body.size()) + "\r\n\r\n" + invalid_body);
  REQUIRE(response.status == 400);
}

TEST_CASE("web admin assets expose install filters and maintenance dialog hooks") {
  const auto web_root = FindRepoWebRoot();
  const auto install_html = ReadBinaryFile(web_root / "install.html");
  const auto agents_html = ReadBinaryFile(web_root / "agents.html");
  const auto admin_js = ReadBinaryFile(web_root / "assets" / "admin.js");

  REQUIRE(install_html.find("data-install-platform") != std::string::npos);
  REQUIRE(install_html.find("data-install-channel") != std::string::npos);
  REQUIRE(install_html.find("data-generate-install-command") !=
          std::string::npos);

  REQUIRE(agents_html.find("data-status-for=\"agents\"") != std::string::npos);
  REQUIRE(agents_html.find("data-maintenance-dialog") != std::string::npos);
  REQUIRE(agents_html.find("data-maintenance-action") != std::string::npos);
  REQUIRE(agents_html.find("data-maintenance-package") != std::string::npos);
  REQUIRE(agents_html.find("data-maintenance-confirm") != std::string::npos);

  REQUIRE(admin_js.find("capability_not_allowed") != std::string::npos);
  REQUIRE(admin_js.find("data-open-maintenance") != std::string::npos);
  REQUIRE(admin_js.find("/api/v1/install/commands?server_url=") !=
          std::string::npos);
}

TEST_CASE("admin http server refuses missing Web static resources") {
  const zfleet::test::ScopedTestDir test_root("server");
  zfleet::server::ServerDatabase database(test_root / "zfleet.db");
  database.Initialize();
  zfleet::server::AdminHttpServer server(
      "127.0.0.1:0",
      &database,
      test_root / "packages",
      test_root / "missing" / "web");

  REQUIRE_THROWS_AS(server.Start(), std::runtime_error);
}

TEST_CASE("admin http server serves requests while an idle connection waits") {
  const zfleet::test::ScopedTestDir test_root("server");
  zfleet::server::ServerDatabase database(test_root / "zfleet.db");
  database.Initialize();
  const auto web_root = test_root / "share" / "web";
  WriteWebStaticFiles(web_root);
  zfleet::server::AdminHttpServer server(
      "127.0.0.1:0",
      &database,
      test_root / "packages",
      web_root);
  server.Start();

  boost::asio::io_context io_context;
  boost::asio::ip::tcp::socket idle_socket(io_context);
  idle_socket.connect(
      {boost::asio::ip::make_address("127.0.0.1"), server.port()});

  const auto page = SendHttpRequest(
      server.port(),
      "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
  REQUIRE(page.status == 200);
  REQUIRE(page.body.find("z-fleet") != std::string::npos);

  idle_socket.close();
  server.Stop();
}

TEST_CASE("admin http server bounds incomplete and oversized requests") {
  const zfleet::test::ScopedTestDir test_root("server");
  zfleet::server::ServerDatabase database(test_root / "zfleet.db");
  database.Initialize();
  const auto web_root = test_root / "share" / "web";
  WriteWebStaticFiles(web_root);
  zfleet::server::AdminHttpServer server(
      "127.0.0.1:0",
      &database,
      test_root / "packages",
      web_root,
      zfleet::server::AdminHttpServerOptions{
          .io_threads = 1,
          .request_timeout = std::chrono::milliseconds(25),
          .max_header_bytes = 128,
          .max_body_bytes = 4,
      });
  server.Start();

  boost::asio::io_context io_context;
  boost::asio::ip::tcp::socket idle_socket(io_context);
  idle_socket.connect(
      {boost::asio::ip::make_address("127.0.0.1"), server.port()});
  const auto timeout = ReadHttpResponse(&idle_socket);
  REQUIRE(timeout.status == 408);

  const auto large_header = SendHttpRequest(
      server.port(),
      "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nX-Long: " +
          std::string(256, 'x') + "\r\n\r\n");
  REQUIRE(large_header.status == 431);

  const auto large_body = SendHttpRequest(
      server.port(),
      "POST /api/v1/install/tokens HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\n12345");
  REQUIRE(large_body.status == 413);

  server.Stop();
}

TEST_CASE("admin http server cleans timed out streamed upload staging") {
  const zfleet::test::ScopedTestDir test_root("server");
  zfleet::server::ServerDatabase database(test_root / "zfleet.db");
  database.Initialize();
  const auto web_root = test_root / "share" / "web";
  WriteWebStaticFiles(web_root);
  zfleet::server::AdminHttpServer server(
      "127.0.0.1:0",
      &database,
      test_root / "packages",
      web_root,
      zfleet::server::AdminHttpServerOptions{
          .io_threads = 1,
          .request_timeout = std::chrono::milliseconds(25),
          .max_header_bytes = 1024,
          .max_body_bytes = 1024,
      });
  server.Start();

  boost::asio::io_context io_context;
  boost::asio::ip::tcp::socket socket(io_context);
  socket.connect({boost::asio::ip::make_address("127.0.0.1"), server.port()});
  const std::string partial_upload =
      "POST /api/v1/admin/packages?platform=linux&arch=x86_64 HTTP/1.1\r\n"
      "Host: 127.0.0.1\r\nContent-Length: 100\r\n\r\npartial";
  boost::asio::write(socket, boost::asio::buffer(partial_upload));
  const auto timeout = ReadHttpResponse(&socket);
  REQUIRE(timeout.status == 408);

  const auto staging_dir = test_root / "packages" / ".staging";
  REQUIRE(std::filesystem::is_directory(staging_dir));
  REQUIRE(std::filesystem::is_empty(staging_dir));

  server.Stop();
}

TEST_CASE("admin http server uploads publishes and resolves package channel") {
  const zfleet::test::ScopedTestDir test_root("server");

  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  const auto web_root = test_root / "share" / "web";
  WriteWebStaticFiles(web_root);

  zfleet::server::AdminHttpServer server(
      "127.0.0.1:0",
      &database,
      test_root / "packages",
      web_root,
      zfleet::server::AdminHttpServerOptions{
          .allow_high_risk_write = true,
          .package_download_base_url = "http://127.0.0.1:8080",
      });
  server.Start();
  SeedAgent(&database, "agent-upgrade-1");
  database.RecordAssetSnapshot(
      zfleet::protocol::AssetSnapshot{
          .protocol_version = "v1",
          .request_id = "asset-agent-upgrade-1",
          .agent_id = "agent-upgrade-1",
          .occurred_at = "2026-05-24T10:00:00Z",
          .hostname = "devbox-01",
          .os = "linux",
          .arch = "x86_64",
          .agent_version = "0.1.0",
      },
      AssetSnapshotEvent("asset-agent-upgrade-1", "agent-upgrade-1",
                         "2026-05-24T10:00:00Z"));

  const auto archive_path =
      CreatePackageArchive(test_root.path(), "agent", "0.2.0", "release");
  const auto archive_body = ReadBinaryFile(archive_path);
  std::string upload_request =
      "POST /api/v1/admin/packages?filename=zfleet_agent-v0.2.0-linux-x86_64-release.zip HTTP/1.1\r\n"
      "Host: 127.0.0.1\r\n"
      "Content-Type: application/zip\r\n"
      "Content-Length: " +
      std::to_string(archive_body.size()) + "\r\n\r\n";
  upload_request += archive_body;

  const auto upload = SendHttpRequest(server.port(), upload_request);
  REQUIRE(upload.status == 201);
  REQUIRE(upload.body.find("\"status\":\"validated\"") != std::string::npos);
  REQUIRE(database.ListAgentPackages().size() == 1);
  REQUIRE(database.ListAgentPackages().front().platform == "linux");
  REQUIRE(database.ListAgentPackages().front().arch == "x86_64");
  REQUIRE(database.ListAgentPackages().front().build_type == "release");
  const auto package_id = database.ListAgentPackages().front().package_id;

  const std::string publish_body = R"json({"channel":"candidate"})json";
  std::string publish_request =
      "POST /api/v1/admin/packages/" + package_id +
      "/publish HTTP/1.1\r\n"
      "Host: 127.0.0.1\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: " +
      std::to_string(publish_body.size()) + "\r\n\r\n" + publish_body;
  const auto publish = SendHttpRequest(server.port(), publish_request);
  REQUIRE(publish.status == 200);
  REQUIRE(publish.body.find("\"status\":\"published\"") != std::string::npos);
  REQUIRE(publish.body.find("\"published_channels\":[\"candidate\"]") !=
          std::string::npos);

  const std::string upgrade_body =
      "{\"package_id\":\"" + package_id + "\",\"set_by\":\"admin\"}";
  std::string upgrade_request =
      "POST /api/v1/agents/agent-upgrade-1/upgrade HTTP/1.1\r\n"
      "Host: 127.0.0.1\r\nContent-Type: application/json\r\nContent-Length: " +
      std::to_string(upgrade_body.size()) + "\r\n\r\n" + upgrade_body;
  const auto upgrade = SendHttpRequest(server.port(), upgrade_request);
  REQUIRE(upgrade.status == 409);
  REQUIRE(upgrade.body.find("installer_too_old") !=
          std::string::npos);

  const auto missing_installer = SendHttpRequest(
      server.port(),
      "GET /api/v1/install/options?channel=candidate&platform=linux&arch=x86_64&build_type=release HTTP/1.1\r\n"
      "Host: 127.0.0.1\r\n\r\n");
  REQUIRE(missing_installer.status == 409);
  REQUIRE(missing_installer.body.find("no_compatible_installer_package") !=
          std::string::npos);

  const auto installer_archive =
      CreatePackageArchive(test_root.path(), "installer", "0.1.0", "release");
  const auto installer_body = ReadBinaryFile(installer_archive);
  std::string installer_upload_request =
      "POST /api/v1/admin/packages?filename=zfleet_installer-v0.1.0-linux-x86_64-release.zip HTTP/1.1\r\n"
      "Host: 127.0.0.1\r\nContent-Type: application/zip\r\nContent-Length: " +
      std::to_string(installer_body.size()) + "\r\n\r\n" + installer_body;
  const auto installer_upload =
      SendHttpRequest(server.port(), installer_upload_request);
  REQUIRE(installer_upload.status == 201);
  const auto records = database.ListAgentPackages();
  const auto installer_it =
      std::find_if(records.begin(), records.end(), [](const auto& record) {
        return record.component == "installer";
      });
  REQUIRE(installer_it != records.end());
  const auto installer_id = installer_it->package_id;
  std::string installer_publish_request =
      "POST /api/v1/admin/packages/" + installer_id +
      "/publish HTTP/1.1\r\nHost: 127.0.0.1\r\n"
      "Content-Type: application/json\r\nContent-Length: " +
      std::to_string(publish_body.size()) + "\r\n\r\n" + publish_body;
  REQUIRE(SendHttpRequest(server.port(), installer_publish_request).status ==
          200);

  const auto chained_upgrade = SendHttpRequest(server.port(), upgrade_request);
  REQUIRE(chained_upgrade.status == 201);
  REQUIRE(chained_upgrade.body.find("\"upgrade_state\":\"queued\"") !=
          std::string::npos);
  REQUIRE(chained_upgrade.body.find("\"prerequisite_task_id\":\"\"") ==
          std::string::npos);
  REQUIRE(ReadAgentField(database_path, "agent-upgrade-1", "desired_version") ==
          "0.2.0");
  REQUIRE(ReadAgentField(database_path, "agent-upgrade-1",
                         "desired_package_id") == package_id);
  REQUIRE(ReadAgentField(database_path, "agent-upgrade-1", "upgrade_state") ==
          "queued");
  const auto task_id =
      ReadAgentField(database_path, "agent-upgrade-1", "last_upgrade_task_id");
  const auto upgrade_task = database.FindTaskById(task_id);
  REQUIRE(upgrade_task.has_value());
  REQUIRE(upgrade_task->task.task_type ==
          zfleet::protocol::TaskType::package_update);
  REQUIRE(upgrade_task->task.capability_level ==
          zfleet::protocol::CapabilityLevel::high_risk_write);
  const auto update_input =
      std::get<zfleet::protocol::PackageUpdateInput>(upgrade_task->task.input);
  REQUIRE(update_input.package_id == package_id);
  REQUIRE(update_input.build_type == "release");
  REQUIRE(update_input.package_url.find("http://127.0.0.1:8080/api/v1/") ==
          0);
  const auto first_task = database.ClaimNextTaskForAgent(
      "agent-upgrade-1", "2026-05-24T11:00:00Z");
  REQUIRE(first_task.has_value());
  const auto first_input =
      std::get<zfleet::protocol::PackageUpdateInput>(first_task->input);
  REQUIRE(first_input.component == "installer");
  REQUIRE_FALSE(database.ClaimNextTaskForAgent(
                    "agent-upgrade-1", "2026-05-24T11:00:01Z")
                    .has_value());
  database.RecordTaskResult(
      zfleet::protocol::TaskResult{
          .protocol_version = "v1",
          .request_id = "installer-complete",
          .task_id = first_task->task_id,
          .agent_id = "agent-upgrade-1",
          .task_type = zfleet::protocol::TaskType::package_update,
          .occurred_at = "2026-05-24T11:00:02Z",
          .status = zfleet::protocol::TaskExecutionStatus::succeeded,
          .result = zfleet::protocol::PackageUpdateResult{
              .component = "installer",
              .package_id = first_input.package_id,
              .version = first_input.version,
              .state = "applied",
          },
      },
      std::nullopt, std::nullopt);
  const auto agent_task = database.ClaimNextTaskForAgent(
      "agent-upgrade-1", "2026-05-24T11:00:03Z");
  REQUIRE(agent_task.has_value());
  REQUIRE(agent_task->task_id == task_id);

  const auto options = SendHttpRequest(
      server.port(),
      "GET /api/v1/install/options?channel=candidate&platform=linux&arch=x86_64&build_type=release HTTP/1.1\r\n"
      "Host: 127.0.0.1\r\n\r\n");
  REQUIRE(options.status == 200);
  REQUIRE(options.body.find(package_id) != std::string::npos);
  REQUIRE(options.body.find(installer_id) != std::string::npos);
  REQUIRE(options.body.find("\"agent\"") != std::string::npos);
  REQUIRE(options.body.find("\"installer\"") != std::string::npos);

  const auto retire = SendHttpRequest(
      server.port(),
      "POST /api/v1/admin/packages/" + package_id +
          "/retire HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 0\r\n\r\n");
  REQUIRE(retire.status == 200);
  REQUIRE(retire.body.find("\"status\":\"retired\"") != std::string::npos);
  REQUIRE(SendHttpRequest(server.port(), publish_request).status == 409);
  const auto no_retired_default = SendHttpRequest(
      server.port(),
      "GET /api/v1/install/options?channel=candidate&platform=linux&arch=x86_64&build_type=release HTTP/1.1\r\n"
      "Host: 127.0.0.1\r\n\r\n");
  REQUIRE(no_retired_default.status == 404);

  const auto debug_archive =
      CreatePackageArchive(test_root.path(), "agent", "0.1.1", "debug");
  const auto debug_body = ReadBinaryFile(debug_archive);
  std::string debug_upload_request =
      "POST /api/v1/admin/packages?filename=zfleet_agent-v0.1.1-linux-x86_64-debug.zip HTTP/1.1\r\n"
      "Host: 127.0.0.1\r\nContent-Type: application/zip\r\nContent-Length: " +
      std::to_string(debug_body.size()) + "\r\n\r\n" + debug_body;
  REQUIRE(SendHttpRequest(server.port(), debug_upload_request).status == 201);
  const auto updated_records = database.ListAgentPackages();
  const auto debug_it =
      std::find_if(updated_records.begin(), updated_records.end(),
                   [](const auto& record) {
                     return record.build_type == "debug";
                   });
  REQUIRE(debug_it != updated_records.end());
  const std::string stable_body = R"json({"channel":"stable"})json";
  std::string stable_request =
      "POST /api/v1/admin/packages/" + debug_it->package_id +
      "/publish HTTP/1.1\r\nHost: 127.0.0.1\r\n"
      "Content-Type: application/json\r\nContent-Length: " +
      std::to_string(stable_body.size()) + "\r\n\r\n" + stable_body;
  const auto stable_debug = SendHttpRequest(server.port(), stable_request);
  REQUIRE(stable_debug.status == 400);
  REQUIRE(stable_debug.body.find("build_type_not_allowed") !=
          std::string::npos);

  server.Stop();
}

TEST_CASE("server database claims queued task only once") {
  const zfleet::test::ScopedTestDir test_root("server");

  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  SeedTask(&database, "task-claim-1", "agent-1");

  const auto first_claim = database.ClaimNextTaskForAgent(
      "agent-1", "2026-05-21T10:00:01Z");
  const auto second_claim = database.ClaimNextTaskForAgent(
      "agent-1", "2026-05-21T10:00:02Z");

  REQUIRE(first_claim.has_value());
  REQUIRE(first_claim->task_id == "task-claim-1");
  REQUIRE_FALSE(second_claim.has_value());
  REQUIRE(ReadTaskField(database_path, "task-claim-1", "state") ==
          "assigned");
}

TEST_CASE("server database serializes concurrent task claims through write actor") {
  const zfleet::test::ScopedTestDir test_root("server");

  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  SeedTask(&database, "task-concurrent-claim-1", "agent-1");

  constexpr int kClaimers = 16;
  std::atomic_bool start{false};
  std::vector<std::future<std::optional<zfleet::protocol::Task>>> claims;
  claims.reserve(kClaimers);
  for (int i = 0; i < kClaimers; ++i) {
    claims.push_back(std::async(std::launch::async, [&database, &start, i]() {
      while (!start.load()) {
        std::this_thread::yield();
      }
      return database.ClaimNextTaskForAgent(
          "agent-1", "2026-05-21T10:00:" + std::to_string(10 + i) + "Z");
    }));
  }

  start.store(true);
  int claimed_count = 0;
  for (auto& claim : claims) {
    const auto task = claim.get();
    if (task.has_value()) {
      ++claimed_count;
      REQUIRE(task->task_id == "task-concurrent-claim-1");
    }
  }

  REQUIRE(claimed_count == 1);
  REQUIRE(ReadTaskField(database_path, "task-concurrent-claim-1", "state") ==
          "assigned");
}

TEST_CASE("server database stop closes write actor and wakes task queue waiters") {
  const zfleet::test::ScopedTestDir test_root("server");

  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  const auto version = database.TaskQueueVersion();

  auto waiter = std::async(std::launch::async, [&database, version]() {
    return database.WaitForTaskQueueChange(version, std::chrono::seconds(30));
  });

  database.Stop();

  REQUIRE(waiter.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
  REQUIRE(waiter.get() == version);
  REQUIRE_THROWS_AS(
      database.EnqueueTask(zfleet::protocol::Task{
          .protocol_version = "v1",
          .task_id = "task-after-stop",
          .agent_id = "agent-1",
          .task_type = zfleet::protocol::TaskType::collect_basic_inventory,
          .capability_level = zfleet::protocol::CapabilityLevel::readonly,
          .created_at = "2026-05-21T10:00:00Z",
          .expires_at = "2099-05-21T10:05:00Z",
          .input = zfleet::protocol::CollectBasicInventoryInput{},
      }),
      std::runtime_error);
}

TEST_CASE("server database async operations post completions to executor") {
  const zfleet::test::ScopedTestDir test_root("server");

  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  boost::asio::io_context io_context;
  auto work_guard = boost::asio::make_work_guard(io_context);
  std::thread io_thread([&io_context]() { io_context.run(); });

  bool enqueue_completed = false;
  std::exception_ptr enqueue_error;
  database.AsyncEnqueueTask(
      zfleet::protocol::Task{
          .protocol_version = "v1",
          .task_id = "task-async-1",
          .agent_id = "agent-async-1",
          .task_type = zfleet::protocol::TaskType::collect_basic_inventory,
          .capability_level = zfleet::protocol::CapabilityLevel::readonly,
          .created_at = "2026-05-21T10:00:00Z",
          .expires_at = "2099-05-21T10:05:00Z",
          .input = zfleet::protocol::CollectBasicInventoryInput{},
      },
      io_context.get_executor(),
      [&](std::exception_ptr error) {
        enqueue_error = error;
        enqueue_completed = true;
        work_guard.reset();
      });

  if (io_thread.joinable()) {
    io_thread.join();
  }

  REQUIRE(enqueue_completed);
  REQUIRE(enqueue_error == nullptr);
  REQUIRE(ReadTaskField(database_path, "task-async-1", "state") == "queued");
}

TEST_CASE("server database task queue subscriptions observe enqueue changes") {
  const zfleet::test::ScopedTestDir test_root("server");

  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  boost::asio::io_context io_context;
  auto work_guard = boost::asio::make_work_guard(io_context);
  std::thread io_thread([&io_context]() { io_context.run(); });

  bool notified = false;
  std::uint64_t observed_version = 0;
  const auto subscription = database.SubscribeTaskQueueChanges(
      io_context.get_executor(),
      [&](std::uint64_t version) {
        observed_version = version;
        notified = true;
        work_guard.reset();
      });

  database.EnqueueTask(zfleet::protocol::Task{
      .protocol_version = "v1",
      .task_id = "task-subscription-1",
      .agent_id = "agent-subscription-1",
      .task_type = zfleet::protocol::TaskType::collect_basic_inventory,
      .capability_level = zfleet::protocol::CapabilityLevel::readonly,
      .created_at = "2026-05-21T10:00:00Z",
      .expires_at = "2099-05-21T10:05:00Z",
      .input = zfleet::protocol::CollectBasicInventoryInput{},
  });

  if (io_thread.joinable()) {
    io_thread.join();
  }

  database.UnsubscribeTaskQueueChanges(subscription);

  REQUIRE(notified);
  REQUIRE(observed_version > 0);
  REQUIRE(ReadTaskField(database_path, "task-subscription-1", "state") ==
          "queued");
}

TEST_CASE("server database async claim keeps single assignment semantics") {
  const zfleet::test::ScopedTestDir test_root("server");

  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  SeedTask(&database, "task-async-claim-1", "agent-async-claim-1");
  boost::asio::io_context io_context;
  auto work_guard = boost::asio::make_work_guard(io_context);
  std::thread io_thread([&io_context]() { io_context.run(); });

  int completion_count = 0;
  int claimed_count = 0;
  std::exception_ptr first_error;
  std::exception_ptr second_error;
  database.AsyncClaimNextTaskForAgent(
      "agent-async-claim-1", "2026-05-21T10:00:01Z",
      io_context.get_executor(),
      [&](std::exception_ptr error,
          std::optional<zfleet::protocol::Task> task) {
        first_error = error;
        if (task.has_value()) {
          ++claimed_count;
        }
        ++completion_count;
        if (completion_count == 2) {
          work_guard.reset();
        }
      });
  database.AsyncClaimNextTaskForAgent(
      "agent-async-claim-1", "2026-05-21T10:00:02Z",
      io_context.get_executor(),
      [&](std::exception_ptr error,
          std::optional<zfleet::protocol::Task> task) {
        second_error = error;
        if (task.has_value()) {
          ++claimed_count;
        }
        ++completion_count;
        if (completion_count == 2) {
          work_guard.reset();
        }
      });

  if (io_thread.joinable()) {
    io_thread.join();
  }

  REQUIRE(completion_count == 2);
  REQUIRE(first_error == nullptr);
  REQUIRE(second_error == nullptr);
  REQUIRE(claimed_count == 1);
  REQUIRE(ReadTaskField(database_path, "task-async-claim-1", "state") ==
          "assigned");
}

TEST_CASE("server database async submission reports stopped actor on executor") {
  const zfleet::test::ScopedTestDir test_root("server");

  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  database.Stop();
  boost::asio::io_context io_context;
  auto work_guard = boost::asio::make_work_guard(io_context);
  std::thread io_thread([&io_context]() { io_context.run(); });

  bool completed = false;
  std::exception_ptr completion_error;
  database.AsyncEnqueueTask(
      zfleet::protocol::Task{
          .protocol_version = "v1",
          .task_id = "task-async-after-stop",
          .agent_id = "agent-async-1",
          .task_type = zfleet::protocol::TaskType::collect_basic_inventory,
          .capability_level = zfleet::protocol::CapabilityLevel::readonly,
          .created_at = "2026-05-21T10:00:00Z",
          .expires_at = "2099-05-21T10:05:00Z",
          .input = zfleet::protocol::CollectBasicInventoryInput{},
      },
      io_context.get_executor(),
      [&](std::exception_ptr error) {
        completion_error = error;
        completed = true;
        work_guard.reset();
      });

  if (io_thread.joinable()) {
    io_thread.join();
  }

  REQUIRE(completed);
  REQUIRE(completion_error != nullptr);
}

TEST_CASE("server database rejects mismatched task type and input payload") {
  const zfleet::test::ScopedTestDir test_root("server");
  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();

  const zfleet::protocol::Task mismatched{
      .protocol_version = "v1",
      .task_id = "task-mismatched-input",
      .agent_id = "agent-1",
      .task_type = zfleet::protocol::TaskType::collect_basic_inventory,
      .capability_level = zfleet::protocol::CapabilityLevel::readonly,
      .created_at = "2026-05-25T10:00:00Z",
      .expires_at = "2099-05-25T10:10:00Z",
      .input = zfleet::protocol::PackageUpdateInput{
          .action = "apply",
          .component = "agent",
      },
  };

  REQUIRE_THROWS(database.EnqueueTask(mismatched));
}

TEST_CASE("control worker pool runs tasks and rejects submissions after stop") {
  zfleet::server::ControlWorkerPool worker_pool(2);

  auto result = worker_pool.Submit([]() {
    return std::vector<zfleet::server::ControlEventResult>{
        zfleet::server::ControlEventResult{
            .status = zfleet::server::ControlEventStatus::kAccepted,
            .message = "ok",
        }};
  });

  const auto events = result.get();
  REQUIRE(events.size() == 1);
  REQUIRE(events[0].status == zfleet::server::ControlEventStatus::kAccepted);
  REQUIRE(events[0].message == "ok");

  worker_pool.Stop();
  REQUIRE_THROWS_AS(
      worker_pool.Submit([]() {
        return std::vector<zfleet::server::ControlEventResult>{};
      }),
      std::runtime_error);
}

TEST_CASE("control worker pool posts async completion to executor") {
  zfleet::server::ControlWorkerPool worker_pool(2);
  boost::asio::io_context io_context;
  auto work_guard = boost::asio::make_work_guard(io_context);
  std::thread io_thread([&io_context]() { io_context.run(); });

  bool completed = false;
  std::exception_ptr completion_error;
  std::vector<zfleet::server::ControlEventResult> completion_results;
  const auto accepted = worker_pool.Submit(
      []() {
        return std::vector<zfleet::server::ControlEventResult>{
            zfleet::server::ControlEventResult{
                .status = zfleet::server::ControlEventStatus::kAccepted,
                .message = "async-ok",
            }};
      },
      io_context.get_executor(),
      [&](std::exception_ptr error,
          std::vector<zfleet::server::ControlEventResult> results) {
        completion_error = error;
        completion_results = std::move(results);
        completed = true;
        work_guard.reset();
      });

  REQUIRE(accepted);
  if (io_thread.joinable()) {
    io_thread.join();
  }

  REQUIRE(completed);
  REQUIRE(completion_error == nullptr);
  REQUIRE(completion_results.size() == 1);
  REQUIRE(completion_results[0].message == "async-ok");

  worker_pool.Stop();
}

TEST_CASE("control service registers agent and accepts heartbeat") {
  const zfleet::test::ScopedTestDir test_root("server");

  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  const zfleet::server::ControlService service(&database);

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
  REQUIRE(CountRows(database_path, "audit_events") == 1);
  REQUIRE(ReadAgentLastSeen(database_path, "agent-1") ==
          "2026-05-21T10:00:00Z");
  REQUIRE(ReadAgentField(database_path, "agent-1", "last_online_at") ==
          "2026-05-21T10:00:00Z");
  REQUIRE(ReadAgentField(database_path, "agent-1", "last_offline_at").empty());
  REQUIRE(ReadAgentField(database_path, "agent-1", "agent_version") ==
          "0.1.0");
  REQUIRE(ReadAgentField(database_path, "agent-1", "status") == "online");
  REQUIRE(ReadAuditField(database_path, "http2-register", "event_type") ==
          "agent.register");
  REQUIRE(ReadAuditField(database_path, "http2-heartbeat", "event_type")
              .empty());

  database.MarkAgentOffline("agent-1", "2026-05-21T10:00:10Z");
  REQUIRE(ReadAgentLastSeen(database_path, "agent-1") ==
          "2026-05-21T10:00:10Z");
  REQUIRE(ReadAgentField(database_path, "agent-1", "last_offline_at") ==
          "2026-05-21T10:00:10Z");
  REQUIRE(ReadAgentField(database_path, "agent-1", "status") == "offline");

  const auto reregister_result = service.HandleAgentEvent(RegisterEvent(
      "http2-reregister", "agent-1", "2026-05-21T10:00:20Z"));
  REQUIRE(reregister_result.status ==
          zfleet::server::ControlEventStatus::kAccepted);
  REQUIRE(ReadAgentLastSeen(database_path, "agent-1") ==
          "2026-05-21T10:00:20Z");
  REQUIRE(ReadAgentField(database_path, "agent-1", "last_online_at") ==
          "2026-05-21T10:00:20Z");
  REQUIRE(ReadAgentField(database_path, "agent-1", "last_offline_at").empty());
  REQUIRE(ReadAgentField(database_path, "agent-1", "status") == "online");
}

TEST_CASE("control service stores asset snapshot display columns and blob") {
  const zfleet::test::ScopedTestDir test_root("server");

  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  SeedAgent(&database, "agent-1");
  const zfleet::server::ControlService service(&database);

  const auto snapshot_result = service.HandleAgentEvent(AssetSnapshotEvent(
      "asset-snapshot-1", "agent-1", "2026-05-21T10:00:06Z"));

  REQUIRE(snapshot_result.status ==
          zfleet::server::ControlEventStatus::kAccepted);
  REQUIRE(snapshot_result.message == "accepted");
  REQUIRE(CountRows(database_path, "asset_snapshots") == 1);
  REQUIRE(ReadAssetSnapshotField(database_path, "agent-1", "hostname") ==
          "devbox-01");
  REQUIRE(ReadAssetSnapshotField(database_path, "agent-1", "os_version") ==
          "6.8");
  const auto stored_summary = database.FindLatestAssetSnapshot("agent-1");
  REQUIRE(stored_summary.has_value());
  REQUIRE((stored_summary->applications == std::vector<std::string>{"cmake"}));
  REQUIRE((stored_summary->services ==
           std::vector<std::string>{"zfleet-agent"}));
  REQUIRE(ReadAuditField(database_path, "asset-snapshot-1", "event_type") ==
          "agent.asset_snapshot");
  proto::AgentEvent stored_snapshot;
  REQUIRE(stored_snapshot.ParseFromString(
      ReadAssetSnapshotBlob(database_path, "agent-1")));
  REQUIRE(stored_snapshot.message_id() == "asset-snapshot-1");
  REQUIRE(stored_snapshot.payload_case() ==
          proto::AgentEvent::kAssetSnapshot);
}

TEST_CASE("control service consumes scoped registration tokens") {
  const zfleet::test::ScopedTestDir test_root("server");
  zfleet::server::ServerDatabase database(test_root / "zfleet.db");
  database.Initialize();
  database.CreateRegistrationToken(zfleet::server::RegistrationTokenRecord{
      .token_id = "token-register",
      .token_hash = zfleet::crypto::Sha256BytesHex("register-once"),
      .purpose = "agent_register",
      .platform = std::optional<std::string>{"linux"},
      .arch = std::optional<std::string>{"x86_64"},
      .max_uses = 1,
      .status = "active",
      .created_at = "2026-05-21T09:00:00Z",
      .expires_at = "2099-05-21T09:00:00Z",
  });
  const zfleet::server::ControlService service(&database);

  const auto accepted = service.HandleAgentEvent(RegisterEvent(
      "token-register-1", "agent-token-1", "2026-05-21T10:00:00Z",
      "register-once"));
  const auto reused = service.HandleAgentEvent(RegisterEvent(
      "token-register-2", "agent-token-2", "2026-05-21T10:00:01Z",
      "register-once"));
  const auto reconnect = service.HandleAgentEvent(RegisterEvent(
      "token-register-3", "agent-token-1", "2026-05-21T10:00:02Z",
      "register-once"));

  REQUIRE(accepted.status == zfleet::server::ControlEventStatus::kAccepted);
  REQUIRE(reused.status == zfleet::server::ControlEventStatus::kInvalidArgument);
  REQUIRE(reconnect.status == zfleet::server::ControlEventStatus::kAccepted);
  REQUIRE(database.AgentExists("agent-token-1"));
  REQUIRE_FALSE(database.AgentExists("agent-token-2"));
  REQUIRE(database.ListRegistrationTokens().front().status == "consumed");
  REQUIRE(database.ListRegistrationTokens().front().use_count == 1);
  REQUIRE(ReadAuditField(test_root / "zfleet.db", "token-register-1",
                         "event_type") == "agent.register");
  REQUIRE(ReadAuditField(test_root / "zfleet.db", "token-register-2",
                         "event_type") == "registration_token.rejected");
}

TEST_CASE("control service rejects invalid and unregistered events") {
  const zfleet::test::ScopedTestDir test_root("server");

  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  const zfleet::server::ControlService service(&database);

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
  REQUIRE(CountRows(database_path, "audit_events") == 0);
}

TEST_CASE("control service stores task running and result events") {
  const zfleet::test::ScopedTestDir test_root("server");

  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  SeedAgent(&database, "agent-1");
  SeedTask(&database, "task-1", "agent-1");
  REQUIRE(database.ClaimNextTaskForAgent("agent-1",
                                         "2026-05-21T10:00:01Z")
              .has_value());
  const zfleet::server::ControlService service(&database);

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
  REQUIRE(ReadAuditField(database_path, "task-1", "event_type") ==
          "task.assigned");
  REQUIRE(ReadAuditField(database_path, "task-1", "result") == "success");
  REQUIRE(ReadAuditField(database_path, "task-running-1", "event_type") ==
          "task.running");
  REQUIRE(ReadAuditField(database_path, "task-running-1", "result") ==
          "success");
  REQUIRE(ReadAuditField(database_path, "task-result-1", "event_type") ==
          "task.succeeded");
  REQUIRE(ReadAuditField(database_path, "task-result-1", "result") ==
          "success");
  proto::CollectBasicInventoryResult stored_result;
  REQUIRE(stored_result.ParseFromString(
      ReadTaskResultBlob(database_path, "task-1", "result_blob")));
  REQUIRE(stored_result.hostname() == "devbox-01");
  REQUIRE(ReadTaskResultBlob(database_path, "task-1", "error_blob").empty());

  const auto repeated_result = service.HandleAgentEvent(TaskSucceededEvent(
      "task-result-1-repeat", "agent-1", "task-1",
      "2026-05-21T10:00:04Z"));
  REQUIRE(repeated_result.status ==
          zfleet::server::ControlEventStatus::kInvalidArgument);
  REQUIRE(repeated_result.message == "task already finished");
  REQUIRE(CountRows(database_path, "task_results") == 1);
}

TEST_CASE("control service stores failed task error columns and blob") {
  const zfleet::test::ScopedTestDir test_root("server");

  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  SeedAgent(&database, "agent-1");
  SeedTask(&database, "task-failed-1", "agent-1");
  REQUIRE(database.ClaimNextTaskForAgent("agent-1",
                                         "2026-05-21T10:00:01Z")
              .has_value());
  const zfleet::server::ControlService service(&database);

  REQUIRE(service.HandleAgentEvent(TaskRunningEvent(
              "task-running-failed-1", "agent-1", "task-failed-1",
              "2026-05-21T10:00:02Z"))
              .status == zfleet::server::ControlEventStatus::kAccepted);
  const auto result = service.HandleAgentEvent(TaskFailedEvent(
      "task-result-failed-1", "agent-1", "task-failed-1",
      "2026-05-21T10:00:03Z"));

  REQUIRE(result.status == zfleet::server::ControlEventStatus::kAccepted);
  REQUIRE(ReadTaskField(database_path, "task-failed-1", "state") == "failed");
  REQUIRE(ReadTaskResultField(database_path, "task-failed-1", "error_code") ==
          "internal_error");
  REQUIRE(ReadTaskResultField(database_path, "task-failed-1",
                              "error_retryable") == "1");
  proto::AgentError stored_error;
  REQUIRE(stored_error.ParseFromString(
      ReadTaskResultBlob(database_path, "task-failed-1", "error_blob")));
  REQUIRE(stored_error.retryable());
  REQUIRE(stored_error.message() == "inventory failed");
  REQUIRE(ReadAuditField(database_path, "task-running-failed-1", "event_type") ==
          "task.running");
  REQUIRE(ReadAuditField(database_path, "task-running-failed-1", "result") ==
          "success");
  REQUIRE(ReadAuditField(database_path, "task-result-failed-1", "event_type") ==
          "task.failed");
  REQUIRE(ReadAuditField(database_path, "task-result-failed-1", "result") ==
          "success");
}

TEST_CASE("agent reconnect confirms desired package version") {
  const zfleet::test::ScopedTestDir test_root("server");
  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  SeedAgent(&database, "agent-upgrade-confirm");
  database.ScheduleAgentUpgrade(
      "agent-upgrade-confirm", "0.2.0", "pkg-confirm",
      std::optional<std::string>{"admin"}, "2026-05-24T10:00:00Z",
      zfleet::protocol::Task{
          .protocol_version = "v1",
          .task_id = "task-confirm",
          .agent_id = "agent-upgrade-confirm",
          .task_type = zfleet::protocol::TaskType::package_update,
          .capability_level = zfleet::protocol::CapabilityLevel::high_risk_write,
          .created_at = "2026-05-24T10:00:00Z",
          .expires_at = "2099-05-24T10:00:00Z",
          .input = zfleet::protocol::PackageUpdateInput{
              .component = "agent",
              .package_id = "pkg-confirm",
              .version = "0.2.0",
          },
      });
  const zfleet::server::ControlService service(&database);
  REQUIRE(service.HandleAgentEvent(RegisterEvent(
              "reconnect-before-apply", "agent-upgrade-confirm",
              "2026-05-24T10:00:00Z"))
              .status == zfleet::server::ControlEventStatus::kAccepted);
  REQUIRE(ReadAgentField(database_path, "agent-upgrade-confirm",
                         "upgrade_state") == "queued");
  REQUIRE(database.ClaimNextTaskForAgent("agent-upgrade-confirm",
                                         "2026-05-24T10:00:01Z")
              .has_value());
  database.RecordTaskResult(
      zfleet::protocol::TaskResult{
          .protocol_version = "v1",
          .request_id = "update-complete",
          .task_id = "task-confirm",
          .agent_id = "agent-upgrade-confirm",
          .task_type = zfleet::protocol::TaskType::package_update,
          .occurred_at = "2026-05-24T10:00:02Z",
          .status = zfleet::protocol::TaskExecutionStatus::succeeded,
          .result = zfleet::protocol::PackageUpdateResult{
              .component = "agent",
              .package_id = "pkg-confirm",
              .version = "0.2.0",
              .state = "applied",
          },
      },
      std::nullopt, std::nullopt);

  REQUIRE(service.HandleAgentEvent(RegisterEvent(
              "reconnect-confirm", "agent-upgrade-confirm",
              "2026-05-24T10:01:00Z", {}, "0.2.0"))
              .status == zfleet::server::ControlEventStatus::kAccepted);
  REQUIRE(ReadAgentField(database_path, "agent-upgrade-confirm",
                         "upgrade_state") == "succeeded");
  REQUIRE(ReadAgentField(database_path, "agent-upgrade-confirm",
                         "current_package_id") == "pkg-confirm");
}

TEST_CASE("agent rollback clears desired target and completes on reconnect") {
  const zfleet::test::ScopedTestDir test_root("server");
  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  SeedAgent(&database, "agent-rollback-confirm");
  database.ScheduleAgentRollback(
      "agent-rollback-confirm", std::optional<std::string>{"admin"},
      "2026-05-24T10:00:00Z",
      zfleet::protocol::Task{
          .protocol_version = "v1",
          .task_id = "task-rollback",
          .agent_id = "agent-rollback-confirm",
          .task_type = zfleet::protocol::TaskType::package_update,
          .capability_level = zfleet::protocol::CapabilityLevel::high_risk_write,
          .created_at = "2026-05-24T10:00:00Z",
          .expires_at = "2099-05-24T10:00:00Z",
          .input = zfleet::protocol::PackageUpdateInput{
              .action = "rollback",
              .component = "agent",
          },
      });
  REQUIRE(ReadAgentField(database_path, "agent-rollback-confirm",
                         "desired_version")
              .empty());
  REQUIRE(database.ClaimNextTaskForAgent("agent-rollback-confirm",
                                         "2026-05-24T10:00:01Z")
              .has_value());
  database.RecordTaskResult(
      zfleet::protocol::TaskResult{
          .protocol_version = "v1",
          .request_id = "rollback-complete",
          .task_id = "task-rollback",
          .agent_id = "agent-rollback-confirm",
          .task_type = zfleet::protocol::TaskType::package_update,
          .occurred_at = "2026-05-24T10:00:02Z",
          .status = zfleet::protocol::TaskExecutionStatus::succeeded,
          .result = zfleet::protocol::PackageUpdateResult{
              .component = "agent",
              .state = "applied",
          },
      },
      std::nullopt, std::nullopt);
  REQUIRE(ReadAgentField(database_path, "agent-rollback-confirm",
                         "upgrade_state") == "waiting_reconnect");

  const zfleet::server::ControlService service(&database);
  REQUIRE(service.HandleAgentEvent(RegisterEvent(
              "reconnect-rollback", "agent-rollback-confirm",
              "2026-05-24T10:01:00Z"))
              .status == zfleet::server::ControlEventStatus::kAccepted);
  REQUIRE(ReadAgentField(database_path, "agent-rollback-confirm",
                         "upgrade_state") == "succeeded");

  database.ScheduleAgentRollback(
      "agent-rollback-confirm", std::optional<std::string>{"admin"},
      "2026-05-24T11:00:00Z",
      zfleet::protocol::Task{
          .protocol_version = "v1",
          .task_id = "task-rollback-timeout",
          .agent_id = "agent-rollback-confirm",
          .task_type = zfleet::protocol::TaskType::package_update,
          .capability_level = zfleet::protocol::CapabilityLevel::high_risk_write,
          .created_at = "2026-05-24T11:00:00Z",
          .expires_at = "2099-05-24T10:00:00Z",
          .input = zfleet::protocol::PackageUpdateInput{
              .action = "rollback",
              .component = "agent",
          },
      });
  REQUIRE(database.ClaimNextTaskForAgent("agent-rollback-confirm",
                                         "2026-05-24T11:00:01Z")
              .has_value());
  database.RecordTaskResult(
      zfleet::protocol::TaskResult{
          .protocol_version = "v1",
          .request_id = "rollback-awaiting-timeout",
          .task_id = "task-rollback-timeout",
          .agent_id = "agent-rollback-confirm",
          .task_type = zfleet::protocol::TaskType::package_update,
          .occurred_at = "2026-05-24T11:00:02Z",
          .status = zfleet::protocol::TaskExecutionStatus::succeeded,
          .result = zfleet::protocol::PackageUpdateResult{
              .component = "agent",
              .state = "applied",
          },
      },
      std::nullopt, std::nullopt);
  REQUIRE(database.ExpireWaitingReconnect("2026-05-24T11:05:00Z").empty());
  REQUIRE(database.ExpireWaitingReconnect("2026-05-24T11:11:00Z").size() ==
          1);
  REQUIRE(ReadAgentField(database_path, "agent-rollback-confirm",
                         "last_upgrade_error") ==
          "waiting_reconnect_timeout");
}

TEST_CASE("control service rejects running event before assignment") {
  const zfleet::test::ScopedTestDir test_root("server");

  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  SeedAgent(&database, "agent-1");
  SeedTask(&database, "task-queued-1", "agent-1");
  const zfleet::server::ControlService service(&database);

  const auto running_result = service.HandleAgentEvent(TaskRunningEvent(
      "task-running-queued-1", "agent-1", "task-queued-1",
      "2026-05-21T10:00:02Z"));

  REQUIRE(running_result.status ==
          zfleet::server::ControlEventStatus::kInvalidArgument);
  REQUIRE(running_result.message == "task is not assigned");
  REQUIRE(ReadTaskField(database_path, "task-queued-1", "state") == "queued");
  REQUIRE(CountRows(database_path, "audit_events") == 0);
}

TEST_CASE("control connection registry tracks active heartbeat ownership") {
  zfleet::server::ControlConnectionRegistry registry;

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
  REQUIRE_FALSE(old_connection.has_value());
  REQUIRE(current_connection.has_value());
  REQUIRE(current_connection->connection_id == "conn-2");
  REQUIRE(registry.ActiveConnectionCount() == 1);

  const auto closed =
      registry.CloseConnection("conn-2", "2026-05-21T10:00:20Z");

  REQUIRE(closed.has_value());
  REQUIRE(closed->agent_id == "agent-1");
  REQUIRE(closed->was_current_agent_connection);
  REQUIRE_FALSE(registry.FindByAgent("agent-1").has_value());
  REQUIRE_FALSE(registry.FindByConnection("conn-2").has_value());
  REQUIRE(registry.ActiveConnectionCount() == 0);
}

TEST_CASE("control dispatcher decodes framed protobuf event stream") {
  const zfleet::test::ScopedTestDir test_root("server");

  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  zfleet::server::ControlService service(&database);
  zfleet::server::ControlConnectionRegistry registry;
  registry.OpenConnection("conn-1", "2026-05-21T11:00:00Z");
  zfleet::server::ControlDispatcher dispatcher(
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
  REQUIRE(ReadAgentLastSeen(database_path, "agent-1") ==
          "2026-05-21T11:00:00Z");
  REQUIRE(ReadAgentField(database_path, "agent-1", "last_online_at") ==
          "2026-05-21T11:00:00Z");
  const auto connection = registry.FindByAgent("agent-1");
  REQUIRE(connection.has_value());
  REQUIRE(connection->connection_id == "conn-1");
  REQUIRE(connection->last_heartbeat_at == "2026-05-21T11:00:05Z");
  REQUIRE(registry.IsAgentOnline("agent-1", "2026-05-21T11:00:04Z"));
}
