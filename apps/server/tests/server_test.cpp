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
          .schema_version = 2,
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
              .launchable = true,
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

