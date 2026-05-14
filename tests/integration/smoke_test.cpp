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
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <string>
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

std::filesystem::path MakeTestRoot() {
  return std::filesystem::temp_directory_path() / "zfleet-integration-tests" /
         "server-http";
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

TEST_CASE("register heartbeat and assets requests complete end to end") {
  namespace fs = std::filesystem;

  const auto test_root = MakeTestRoot();
  fs::remove_all(test_root);
  fs::create_directories(test_root);
  const auto database_path = test_root / "zfleet.db";

  {
    RunningServer server(database_path);

    const auto register_response = PostJson(
        server.server.port(), "/v1/agents/register",
        R"json({
          "protocol_version": "v1",
          "request_id": "req-register",
          "agent_id": "agent-1",
          "occurred_at": "2026-05-14T12:00:00Z",
          "agent_version": "0.1.0",
          "hostname": "devbox-01",
          "os": "linux",
          "arch": "x86_64"
        })json");
    const auto heartbeat_response = PostJson(
        server.server.port(), "/v1/agents/agent-1/heartbeat",
        R"json({
          "protocol_version": "v1",
          "request_id": "req-heartbeat",
          "agent_id": "agent-1",
          "occurred_at": "2026-05-14T12:00:01Z",
          "agent_version": "0.1.0"
        })json");
    const auto assets_response = PostJson(
        server.server.port(), "/v1/agents/agent-1/assets",
        R"json({
          "protocol_version": "v1",
          "request_id": "req-assets",
          "agent_id": "agent-1",
          "occurred_at": "2026-05-14T12:00:02Z",
          "hostname": "devbox-01",
          "os": "linux",
          "arch": "x86_64",
          "agent_version": "0.1.0"
        })json");

    REQUIRE(register_response.result() == http::status::ok);
    REQUIRE(heartbeat_response.result() == http::status::ok);
    REQUIRE(assets_response.result() == http::status::ok);
    REQUIRE(CountRows(database_path, "agents") == 1);
    REQUIRE(CountRows(database_path, "heartbeats") == 1);
    REQUIRE(CountRows(database_path, "asset_snapshots") == 1);
  }

  fs::remove_all(test_root);
}
