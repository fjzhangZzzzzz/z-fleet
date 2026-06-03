#include "database.h"
#include "control_connection_registry.h"
#include "control_server.h"
#include "control_service.h"
#include "admin_http_server.h"

#include "test_util.h"
#include "http2_test_util.h"
#include "server_event_test_util.h"

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/asio/write.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

void WriteWebStaticFiles(const std::filesystem::path& root) {
  zfleet::test::WriteTextFile(root / "index.html", "<title>z-fleet</title>");
  zfleet::test::WriteTextFile(root / "install.html",
                              "<title>z-fleet install</title>");
  zfleet::test::WriteTextFile(root / "agents.html",
                              "<title>z-fleet agents</title>");
  zfleet::test::WriteTextFile(root / "admin" / "packages.html",
                              "<title>z-fleet packages</title>");
  zfleet::test::WriteTextFile(root / "assets" / "admin.js",
                              "window.zfleet = {};");
  zfleet::test::WriteTextFile(root / "assets" / "admin.css", "body{}");
  zfleet::test::WriteTextFile(root / "scripts" / "install" / "linux.sh",
                              "#!/bin/sh\n");
  zfleet::test::WriteTextFile(root / "scripts" / "install" / "windows.ps1",
                              "Write-Host z-fleet\n");
}

std::string ReadTimeoutResponse(boost::asio::ip::tcp::socket* socket) {
  boost::asio::streambuf response;
  boost::system::error_code ec;
  boost::asio::read(*socket, response, ec);
  if (ec != boost::asio::error::eof) {
    throw std::runtime_error("failed to read http response");
  }
  std::istream stream(&response);
  std::string text((std::istreambuf_iterator<char>(stream)),
                   std::istreambuf_iterator<char>());
  const auto header_end = text.find("\r\n\r\n");
  if (header_end == std::string::npos) {
    throw std::runtime_error("malformed http response");
  }
  return text.substr(0, header_end + 4);
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

TEST_CASE("failure matrix rejects invalid and unregistered control events") {
  const zfleet::test::ScopedTestDir test_root("server");
  zfleet::server::ServerDatabase database(test_root / "zfleet.db");
  database.Initialize();
  const zfleet::server::ControlService service(&database);

  const auto invalid = service.HandleAgentEvent(zfleet::test::DomainAgentEvent(
      zfleet::test::HeartbeatEvent("heartbeat-unregistered", "agent-missing",
                                   "2026-05-21T10:00:05Z")));
  REQUIRE(invalid.status == zfleet::server::ControlEventStatus::kNotFound);
  REQUIRE(invalid.message == "agent not registered");
  REQUIRE_FALSE(database.AgentExists("agent-missing"));
}

TEST_CASE("failure matrix rejects running event before assignment") {
  const zfleet::test::ScopedTestDir test_root("server");
  const auto database_path = test_root / "zfleet.db";
  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  zfleet::test::SeedAgent(&database, "agent-1");
  zfleet::test::SeedTask(&database, "task-queued-1", "agent-1");
  const zfleet::server::ControlService service(&database);

  const auto running_result = service.HandleAgentEvent(
      zfleet::test::DomainAgentEvent(
      zfleet::test::TaskRunningEvent("task-running-queued-1", "agent-1",
                                     "task-queued-1",
                                     "2026-05-21T10:00:02Z")));

  REQUIRE(running_result.status ==
          zfleet::server::ControlEventStatus::kInvalidArgument);
  REQUIRE(running_result.message == "task is not assigned");
  REQUIRE(database.ClaimNextTaskForAgent("agent-1", "2026-05-21T10:00:03Z")
              .has_value());
}

TEST_CASE("failure matrix rejects mismatched task type and input payload") {
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
      .input =
          zfleet::protocol::PackageUpdateInput{
              .action = "apply",
              .component = "agent",
          },
  };

  REQUIRE_THROWS(database.EnqueueTask(mismatched));
}

TEST_CASE("failure matrix rejects worker submissions after stop") {
  zfleet::server::ControlWorkerPool worker_pool(2);
  worker_pool.Stop();

  REQUIRE_THROWS_AS(worker_pool.Submit([]() {
    return std::vector<zfleet::server::ControlEventResult>{};
  }),
                    std::runtime_error);
}

TEST_CASE("failure matrix rejects network connections to unavailable ports") {
  boost::asio::io_context io_context;
  boost::asio::ip::tcp::socket socket(io_context);

  REQUIRE_THROWS_AS(
      socket.connect({boost::asio::ip::make_address("127.0.0.1"), 1}),
      boost::system::system_error);
}

TEST_CASE("failure matrix rejects missing web static resources") {
  const zfleet::test::ScopedTestDir test_root("server");
  zfleet::server::ServerDatabase database(test_root / "zfleet.db");
  database.Initialize();
  zfleet::server::AdminHttpServer server("127.0.0.1:0", &database,
                                         test_root / "packages",
                                         test_root / "missing" / "web");

  REQUIRE_THROWS_AS(server.Start(), std::runtime_error);
}

TEST_CASE("failure matrix times out incomplete and oversized admin requests") {
  const zfleet::test::ScopedTestDir test_root("server");
  zfleet::server::ServerDatabase database(test_root / "zfleet.db");
  database.Initialize();
  const auto web_root = FindRepoWebRoot();
  zfleet::server::AdminHttpServer server(
      "127.0.0.1:0", &database, test_root / "packages", web_root,
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
  const auto timeout = ReadTimeoutResponse(&idle_socket);
  REQUIRE(timeout.rfind("HTTP/1.1 408", 0) == 0);

  const auto large_header = zfleet::test::SendHttpRequest(
      server.port(), "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nX-Long: " +
                         std::string(256, 'x') + "\r\n\r\n");
  REQUIRE(large_header.status == 431);

  const auto large_body =
      zfleet::test::SendHttpRequest(server.port(),
                      "POST /api/v1/install/tokens HTTP/1.1\r\nHost: "
                      "x\r\nContent-Length: 5\r\n\r\n12345");
  REQUIRE(large_body.status == 413);

  server.Stop();
}

TEST_CASE("failure matrix cleans timed out streamed upload staging") {
  const zfleet::test::ScopedTestDir test_root("server");
  zfleet::server::ServerDatabase database(test_root / "zfleet.db");
  database.Initialize();
  const auto web_root = FindRepoWebRoot();
  zfleet::server::AdminHttpServer server(
      "127.0.0.1:0", &database, test_root / "packages", web_root,
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
  const auto timeout = ReadTimeoutResponse(&socket);
  REQUIRE(timeout.rfind("HTTP/1.1 408", 0) == 0);

  const auto staging_dir = test_root / "packages" / ".staging";
  REQUIRE(std::filesystem::is_directory(staging_dir));
  REQUIRE(std::filesystem::is_empty(staging_dir));

  server.Stop();
}

}  // namespace
