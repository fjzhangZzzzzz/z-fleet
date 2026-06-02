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

#include <algorithm>
#include <array>
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

std::string ReadAuditField(const std::filesystem::path& database_path,
                           const std::string& request_id,
                           const std::string& column_name) {
  SQLite::Database db(database_path.string(), SQLite::OPEN_READONLY);
  SQLite::Statement query(db, "select " + column_name +
                                  " from audit_events where request_id = ? "
                                  "order by rowid desc limit 1");
  query.bind(1, request_id);
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

int CountRows(const std::filesystem::path& database_path,
              const std::string& table_name) {
  SQLite::Database db(database_path.string(), SQLite::OPEN_READONLY);
  SQLite::Statement query(db, "select count(*) from " + table_name);
  query.executeStep();
  return query.getColumn(0).getInt();
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

  const auto manifest =
      zfleet::package::SerializeManifestJson(zfleet::package::Manifest{
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
  zfleet::test::WriteTextFile(package_dir / "META" / "manifest.json", manifest);

  const auto archive_path = root / (component + ".zip");
  zfleet::package::CreateArchive(zfleet::package::CreateArchiveOptions{
      .package_dir = package_dir,
      .archive_path = archive_path,
      .force = true,
  });
  return archive_path;
}

void SeedAgent(zfleet::server::ServerDatabase* database,
               const std::string& agent_id) {
  database->UpsertAgent(zfleet::protocol::AgentRegistration{
      .protocol_version = "v1",
      .request_id = "seed-agent",
      .agent_id = agent_id,
      .occurred_at = "2026-05-24T10:00:00Z",
      .agent_version = "0.1.0",
      .hostname = "devbox-01",
      .os = "linux",
      .arch = "x86_64",
  });
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

HttpResponse SendHttpRequest(std::uint16_t port, const std::string& request) {
  boost::asio::io_context io_context;
  boost::asio::ip::tcp::socket socket(io_context);
  socket.connect({boost::asio::ip::make_address("127.0.0.1"), port});
  boost::asio::write(socket, boost::asio::buffer(request));
  return ReadHttpResponse(&socket);
}

TEST_CASE("admin http server serves static UI and agent api") {
  const zfleet::test::ScopedTestDir test_root("server");

  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  SeedAgent(&database, "agent-http-1");
  database.RecordAssetSnapshot(
      zfleet::protocol::AssetSnapshot{
          .protocol_version = "v1",
          .request_id = "asset-http-1",
          .agent_id = "agent-http-1",
          .occurred_at = "2026-05-24T10:00:00Z",
          .hostname = "devbox-01",
          .os = "linux",
          .os_version = "6.8",
          .arch = "x86_64",
          .agent_version = "0.1.0",
          .applications = {"cmake"},
          .services = {"zfleet-agent"},
      },
      AssetSnapshotEvent("asset-http-1", "agent-http-1",
                         "2026-05-24T10:00:00Z"));
  const auto web_root = test_root / "share" / "web";
  WriteWebStaticFiles(web_root);

  zfleet::server::AdminHttpServer server(
      "127.0.0.1:0", &database, test_root / "packages", web_root,
      zfleet::server::AdminHttpServerOptions{
          .allow_high_risk_write = true,
          .control_url = "http://127.0.0.1:8081",
      });
  server.Start();

  const auto index = SendHttpRequest(
      server.port(), "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
  REQUIRE(index.status == 200);
  REQUIRE(index.headers.find("text/html") != std::string::npos);
  REQUIRE(index.body.find("z-fleet") != std::string::npos);

  const auto page = SendHttpRequest(
      server.port(), "GET /install HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
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
      server.port(), "GET /api/v1/agents HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
  REQUIRE(agents.status == 200);
  REQUIRE(agents.body.find("agent-http-1") != std::string::npos);
  REQUIRE(agents.body.find("\"latest_asset\":{") != std::string::npos);

  const auto agent_detail = SendHttpRequest(
      server.port(),
      "GET /api/v1/agents/agent-http-1 HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
  REQUIRE(agent_detail.status == 200);
  REQUIRE(agent_detail.body.find("\"agent_id\":\"agent-http-1\"") !=
          std::string::npos);
  REQUIRE(agent_detail.body.find("\"latest_asset\":{") != std::string::npos);

  const auto asset_list =
      SendHttpRequest(server.port(),
                      "GET /api/v1/agents/agent-http-1/assets "
                      "HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
  REQUIRE(asset_list.status == 200);
  REQUIRE(asset_list.body.find("\"assets\"") != std::string::npos);

  const auto latest_asset =
      SendHttpRequest(server.port(),
                      "GET /api/v1/agents/agent-http-1/assets/latest "
                      "HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
  REQUIRE(latest_asset.status == 200);
  REQUIRE(latest_asset.body.find("\"snapshot_id\"") != std::string::npos);
  REQUIRE(latest_asset.body.find("\"agent_id\":\"agent-http-1\"") !=
          std::string::npos);

  const auto linux_script =
      SendHttpRequest(server.port(),
                      "GET /api/v1/install/script?platform=linux "
                      "HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
  REQUIRE(linux_script.status == 200);
  REQUIRE(linux_script.body.find("sha256sum -c") != std::string::npos);

  const auto windows_script =
      SendHttpRequest(server.port(),
                      "GET /api/v1/install/script?platform=windows "
                      "HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
  REQUIRE(windows_script.status == 200);
  REQUIRE(windows_script.body.find("Get-FileHash") != std::string::npos);

  const auto install_commands = SendHttpRequest(
      server.port(),
      "GET "
      "/api/v1/install/"
      "commands?server_url=http%3A%2F%2F127.0.0.1%3A8080&token=abc&channel="
      "stable&platform=linux HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
  REQUIRE(install_commands.status == 200);
  REQUIRE(install_commands.body.find("\"command\"") != std::string::npos);
  REQUIRE(install_commands.body.find("\"platform\":\"linux\"") !=
          std::string::npos);
  REQUIRE(install_commands.body.find("/api/v1/install/script?platform=linux") !=
          std::string::npos);
  REQUIRE(install_commands.body.find("--control-url") != std::string::npos);

  const auto install_commands_windows = SendHttpRequest(
      server.port(),
      "GET "
      "/api/v1/install/"
      "commands?server_url=http%3A%2F%2F127.0.0.1%3A8080&token=abc&channel="
      "stable&platform=windows HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
  REQUIRE(install_commands_windows.status == 200);
  REQUIRE(install_commands_windows.body.find("\"platform\":\"windows\"") !=
          std::string::npos);
  REQUIRE(install_commands_windows.body.find(
              "/api/v1/install/script?platform=windows") != std::string::npos);

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
      "127.0.0.1:0", &database, test_root / "packages", web_root,
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
      "127.0.0.1:0", &database, test_root / "packages", web_root,
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

TEST_CASE(
    "web admin assets expose install filters and maintenance dialog hooks") {
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
  zfleet::server::AdminHttpServer server("127.0.0.1:0", &database,
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
  zfleet::server::AdminHttpServer server("127.0.0.1:0", &database,
                                         test_root / "packages", web_root);
  server.Start();

  boost::asio::io_context io_context;
  boost::asio::ip::tcp::socket idle_socket(io_context);
  idle_socket.connect(
      {boost::asio::ip::make_address("127.0.0.1"), server.port()});

  const auto page = SendHttpRequest(
      server.port(), "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
  REQUIRE(page.status == 200);
  REQUIRE(page.body.find("z-fleet") != std::string::npos);

  idle_socket.close();
  server.Stop();
}

TEST_CASE("admin http server stops cleanly with multithreaded io") {
  const zfleet::test::ScopedTestDir test_root("server");
  zfleet::server::ServerDatabase database(test_root / "zfleet.db");
  database.Initialize();
  const auto web_root = test_root / "share" / "web";
  WriteWebStaticFiles(web_root);

  for (int attempt = 0; attempt < 25; ++attempt) {
    zfleet::server::AdminHttpServer server(
        "127.0.0.1:0", &database, test_root / "packages", web_root,
        zfleet::server::AdminHttpServerOptions{.io_threads = 2});
    server.Start();

    boost::asio::io_context io_context;
    boost::asio::ip::tcp::socket idle_socket(io_context);
    idle_socket.connect(
        {boost::asio::ip::make_address("127.0.0.1"), server.port()});

    const auto page = SendHttpRequest(
        server.port(), "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
    REQUIRE(page.status == 200);

    idle_socket.close();
    server.Stop();
  }
}

TEST_CASE("admin http server uploads publishes and resolves package channel") {
  const zfleet::test::ScopedTestDir test_root("server");

  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  const auto web_root = test_root / "share" / "web";
  WriteWebStaticFiles(web_root);

  zfleet::server::AdminHttpServer server(
      "127.0.0.1:0", &database, test_root / "packages", web_root,
      zfleet::server::AdminHttpServerOptions{
          .allow_high_risk_write = true,
          .package_download_base_url = "http://127.0.0.1:8080",
          .control_url = "http://127.0.0.1:8081",
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
      "POST "
      "/api/v1/admin/"
      "packages?filename=zfleet_agent-v0.2.0-linux-x86_64-release.zip "
      "HTTP/1.1\r\n"
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
  std::string publish_request = "POST /api/v1/admin/packages/" + package_id +
                                "/publish HTTP/1.1\r\n"
                                "Host: 127.0.0.1\r\n"
                                "Content-Type: application/json\r\n"
                                "Content-Length: " +
                                std::to_string(publish_body.size()) +
                                "\r\n\r\n" + publish_body;
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
  REQUIRE(upgrade.body.find("installer_too_old") != std::string::npos);

  const auto missing_installer =
      SendHttpRequest(server.port(),
                      "GET "
                      "/api/v1/install/"
                      "options?channel=candidate&platform=linux&arch=x86_64&"
                      "build_type=release HTTP/1.1\r\n"
                      "Host: 127.0.0.1\r\n\r\n");
  REQUIRE(missing_installer.status == 409);
  REQUIRE(missing_installer.body.find("no_compatible_installer_package") !=
          std::string::npos);

  const auto installer_archive =
      CreatePackageArchive(test_root.path(), "installer", "0.1.0", "release");
  const auto installer_body = ReadBinaryFile(installer_archive);
  std::string installer_upload_request =
      "POST "
      "/api/v1/admin/"
      "packages?filename=zfleet_installer-v0.1.0-linux-x86_64-release.zip "
      "HTTP/1.1\r\n"
      "Host: 127.0.0.1\r\nContent-Type: application/zip\r\nContent-Length: " +
      std::to_string(installer_body.size()) + "\r\n\r\n" + installer_body;
  const auto installer_upload =
      SendHttpRequest(server.port(), installer_upload_request);
  REQUIRE(installer_upload.status == 201);
  const auto records = database.ListAgentPackages();
  const auto installer_it = std::find_if(
      records.begin(), records.end(),
      [](const auto& record) { return record.component == "installer"; });
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
  REQUIRE(update_input.package_url.find("http://127.0.0.1:8080/api/v1/") == 0);
  const auto first_task =
      database.ClaimNextTaskForAgent("agent-upgrade-1", "2026-05-24T11:00:00Z");
  REQUIRE(first_task.has_value());
  const auto first_input =
      std::get<zfleet::protocol::PackageUpdateInput>(first_task->input);
  REQUIRE(first_input.component == "installer");
  REQUIRE_FALSE(
      database.ClaimNextTaskForAgent("agent-upgrade-1", "2026-05-24T11:00:01Z")
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
          .result =
              zfleet::protocol::PackageUpdateResult{
                  .component = "installer",
                  .package_id = first_input.package_id,
                  .version = first_input.version,
                  .state = "applied",
              },
      },
      std::nullopt, std::nullopt);
  const auto agent_task =
      database.ClaimNextTaskForAgent("agent-upgrade-1", "2026-05-24T11:00:03Z");
  REQUIRE(agent_task.has_value());
  REQUIRE(agent_task->task_id == task_id);

  const auto options =
      SendHttpRequest(server.port(),
                      "GET "
                      "/api/v1/install/"
                      "options?channel=candidate&platform=linux&arch=x86_64&"
                      "build_type=release HTTP/1.1\r\n"
                      "Host: 127.0.0.1\r\n\r\n");
  REQUIRE(options.status == 200);
  REQUIRE(options.body.find("\"agent\"") != std::string::npos);
  REQUIRE(options.body.find("\"installer\"") != std::string::npos);

  const auto install_commands = SendHttpRequest(
      server.port(),
      "GET "
      "/api/v1/install/"
      "commands?server_url=http%3A%2F%2F127.0.0.1%3A8080&token=abc&channel="
      "candidate&platform=linux HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
  REQUIRE(install_commands.status == 200);
  REQUIRE(install_commands.body.find("\"command\"") != std::string::npos);
  REQUIRE(install_commands.body.find("\"platform\":\"linux\"") !=
          std::string::npos);
  REQUIRE(install_commands.body.find("/api/v1/install/script?platform=linux") !=
          std::string::npos);
  REQUIRE(install_commands.body.find("--control-url") != std::string::npos);

  server.Stop();
}

}  // namespace

