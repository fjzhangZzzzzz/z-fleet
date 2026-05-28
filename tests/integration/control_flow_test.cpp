#include "database.h"
#include "http2_control_client.h"
#include "control_connection_registry.h"
#include "control_dispatcher.h"
#include "control_server.h"
#include "control_service.h"

#include "test_util.h"

#include "zfleet/core/time.h"
#include "zfleet/platform/system.h"
#include "zfleet/protocol/v1/agent_control.pb.h"
#include "zfleet/transport/frame_codec.h"
#include "zfleet/transport/nghttp2_compat.h"

#include <SQLiteCpp/SQLiteCpp.h>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/asio/write.hpp>
#include <catch2/catch_test_macros.hpp>
#include <sqlite3.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <functional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace proto = zfleet::protocol::v1;
namespace asio = boost::asio;
using tcp = asio::ip::tcp;

constexpr int kSqliteReadAttempts = 50;
constexpr auto kSqliteReadRetryDelay = std::chrono::milliseconds(20);

#define ZFLEET_TEST_NGHTTP2_NV(NAME, VALUE)                        \
  nghttp2_nv {                                                     \
    reinterpret_cast<std::uint8_t*>(const_cast<char*>(NAME)),      \
        reinterpret_cast<std::uint8_t*>(const_cast<char*>(VALUE)), \
        sizeof(NAME) - 1, sizeof(VALUE) - 1, NGHTTP2_NV_FLAG_NONE  \
  }

struct ClientBody {
  std::vector<std::uint8_t> bytes;
  std::size_t offset = 0;
};

struct ClientContext {
  tcp::socket socket;
  std::string response_status;
  std::vector<std::uint8_t> response_body;
  bool response_done = false;

  explicit ClientContext(asio::io_context& io_context) : socket(io_context) {}
};

struct HttpResponse {
  int status = 0;
  std::string body;
};

void CheckNghttp2(int rv, std::string_view operation) {
  if (rv < 0) {
    throw std::runtime_error(std::string(operation) +
                             " failed: " + nghttp2_strerror(rv));
  }
}

ssize_t ClientSendCallback(nghttp2_session* /*session*/,
                           const std::uint8_t* data, std::size_t length,
                           int /*flags*/, void* user_data) {
  auto* context = static_cast<ClientContext*>(user_data);
  boost::asio::write(context->socket, boost::asio::buffer(data, length));
  return static_cast<ssize_t>(length);
}

ssize_t ClientReadCallback(nghttp2_session* /*session*/,
                           std::int32_t /*stream_id*/, std::uint8_t* buffer,
                           std::size_t length, std::uint32_t* data_flags,
                           nghttp2_data_source* source, void* /*user_data*/) {
  auto* body = static_cast<ClientBody*>(source->ptr);
  const auto remaining = body->bytes.size() - body->offset;
  const auto bytes_to_copy = std::min(length, remaining);
  if (bytes_to_copy > 0) {
    std::memcpy(buffer, body->bytes.data() + body->offset, bytes_to_copy);
    body->offset += bytes_to_copy;
  }
  if (body->offset == body->bytes.size()) {
    *data_flags |= NGHTTP2_DATA_FLAG_EOF;
  }
  return static_cast<ssize_t>(bytes_to_copy);
}

int ClientHeaderCallback(nghttp2_session* /*session*/,
                         const nghttp2_frame* frame, const std::uint8_t* name,
                         std::size_t name_length, const std::uint8_t* value,
                         std::size_t value_length, std::uint8_t /*flags*/,
                         void* user_data) {
  if (frame->hd.type != NGHTTP2_HEADERS ||
      frame->headers.cat != NGHTTP2_HCAT_RESPONSE) {
    return 0;
  }

  const std::string_view header_name(reinterpret_cast<const char*>(name),
                                     name_length);
  if (header_name == ":status") {
    auto* context = static_cast<ClientContext*>(user_data);
    context->response_status.assign(reinterpret_cast<const char*>(value),
                                    value_length);
  }
  return 0;
}

int ClientFrameRecvCallback(nghttp2_session* /*session*/,
                            const nghttp2_frame* frame, void* user_data) {
  if (((frame->hd.type == NGHTTP2_HEADERS &&
        frame->headers.cat == NGHTTP2_HCAT_RESPONSE) ||
       frame->hd.type == NGHTTP2_DATA) &&
      (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) != 0) {
    auto* context = static_cast<ClientContext*>(user_data);
    context->response_done = true;
  }
  return 0;
}

int ClientDataChunkRecvCallback(nghttp2_session* /*session*/,
                                std::uint8_t /*flags*/,
                                std::int32_t /*stream_id*/,
                                const std::uint8_t* data, std::size_t length,
                                void* user_data) {
  auto* context = static_cast<ClientContext*>(user_data);
  context->response_body.insert(context->response_body.end(), data,
                                data + length);
  return 0;
}

HttpResponse SendHttpRequest(std::uint16_t port, const std::string& request) {
  asio::io_context io_context;
  tcp::socket socket(io_context);
  socket.connect({asio::ip::make_address("127.0.0.1"), port});
  asio::write(socket, asio::buffer(request));

  asio::streambuf response;
  boost::system::error_code ec;
  asio::read(socket, response, ec);
  if (ec != asio::error::eof) {
    throw std::runtime_error("failed to read http response");
  }

  std::istream stream(&response);
  std::string text((std::istreambuf_iterator<char>(stream)),
                   std::istreambuf_iterator<char>());
  const auto header_end = text.find("\r\n\r\n");
  const auto status_end = text.find("\r\n");
  if (header_end == std::string::npos || status_end == std::string::npos) {
    throw std::runtime_error("malformed http response");
  }
  HttpResponse parsed;
  parsed.status = std::stoi(text.substr(9, 3));
  parsed.body = text.substr(header_end + 4);
  return parsed;
}

struct ClientCallbacks {
  nghttp2_session_callbacks* callbacks = nullptr;

  ClientCallbacks() {
    CheckNghttp2(nghttp2_session_callbacks_new(&callbacks),
                 "create client callbacks");
    nghttp2_session_callbacks_set_send_callback(callbacks, ClientSendCallback);
    nghttp2_session_callbacks_set_on_header_callback(callbacks,
                                                     ClientHeaderCallback);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(
        callbacks, ClientDataChunkRecvCallback);
    nghttp2_session_callbacks_set_on_frame_recv_callback(
        callbacks, ClientFrameRecvCallback);
  }

  ~ClientCallbacks() { nghttp2_session_callbacks_del(callbacks); }
};

struct ClientSession {
  nghttp2_session* session = nullptr;

  ClientSession(const ClientCallbacks& callbacks, ClientContext* context) {
    CheckNghttp2(
        nghttp2_session_client_new(&session, callbacks.callbacks, context),
        "create client session");
    CheckNghttp2(
        nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, nullptr, 0),
        "submit client settings");
  }

  ~ClientSession() { nghttp2_session_del(session); }
};

SQLite::Database OpenTestDatabase(const std::filesystem::path& database_path,
                                  const int flags) {
  SQLite::Database db(database_path.string(), flags);
  db.exec("PRAGMA busy_timeout=5000");
  return db;
}

bool IsRecoverableBusy(const SQLite::Exception& ex) {
  return ex.getErrorCode() == SQLITE_BUSY || ex.getErrorCode() == SQLITE_LOCKED;
}

int CountRows(const std::filesystem::path& database_path,
              const std::string& table_name) {
  for (int attempt = 0; attempt < kSqliteReadAttempts; ++attempt) {
    try {
      auto db = OpenTestDatabase(database_path, SQLite::OPEN_READONLY);
      SQLite::Statement query(db, "select count(*) from " + table_name);
      query.executeStep();
      return query.getColumn(0).getInt();
    } catch (const SQLite::Exception& ex) {
      if (!IsRecoverableBusy(ex) || attempt + 1 == kSqliteReadAttempts) {
        throw;
      }
      std::this_thread::sleep_for(kSqliteReadRetryDelay);
    }
  }
  throw std::runtime_error("failed to count rows");
}

int CountByQuery(const std::filesystem::path& database_path,
                 const std::string& query_text) {
  for (int attempt = 0; attempt < kSqliteReadAttempts; ++attempt) {
    try {
      auto db = OpenTestDatabase(database_path, SQLite::OPEN_READONLY);
      SQLite::Statement query(db, query_text);
      query.executeStep();
      return query.getColumn(0).getInt();
    } catch (const SQLite::Exception& ex) {
      if (!IsRecoverableBusy(ex) || attempt + 1 == kSqliteReadAttempts) {
        throw;
      }
      std::this_thread::sleep_for(kSqliteReadRetryDelay);
    }
  }
  throw std::runtime_error("failed to count query rows");
}

std::string ReadSingleTextColumn(const std::filesystem::path& database_path,
                                 const std::string& query_text,
                                 const std::string& parameter) {
  for (int attempt = 0; attempt < kSqliteReadAttempts; ++attempt) {
    try {
      auto db = OpenTestDatabase(database_path, SQLite::OPEN_READONLY);
      SQLite::Statement query(db, query_text);
      query.bind(1, parameter);
      if (!query.executeStep()) {
        return {};
      }
      return query.getColumn(0).getString();
    } catch (const SQLite::Exception& ex) {
      if (!IsRecoverableBusy(ex) || attempt + 1 == kSqliteReadAttempts) {
        throw;
      }
      std::this_thread::sleep_for(kSqliteReadRetryDelay);
    }
  }
  throw std::runtime_error("failed to read text column");
}

std::size_t CountCompleteFrames(const std::vector<std::uint8_t>& bytes) {
  std::size_t offset = 0;
  std::size_t count = 0;
  while (bytes.size() >= offset + 4) {
    const auto length = (static_cast<std::uint32_t>(bytes[offset]) << 24) |
                        (static_cast<std::uint32_t>(bytes[offset + 1]) << 16) |
                        (static_cast<std::uint32_t>(bytes[offset + 2]) << 8) |
                        static_cast<std::uint32_t>(bytes[offset + 3]);
    const auto frame_size = 4 + static_cast<std::size_t>(length);
    if (bytes.size() < offset + frame_size) {
      break;
    }
    offset += frame_size;
    ++count;
  }
  return count;
}

proto::AgentEvent RegisterEvent(std::string message_id, std::string agent_id,
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

proto::AgentEvent HeartbeatEvent(std::string message_id, std::string agent_id,
                                 std::string occurred_at) {
  proto::AgentEvent event;
  event.set_protocol_version("v1");
  event.set_message_id(std::move(message_id));
  event.set_agent_id(std::move(agent_id));
  event.set_occurred_at(std::move(occurred_at));
  event.mutable_heartbeat()->set_agent_version("0.1.0");
  return event;
}

proto::AgentEvent TaskRunningEvent(std::string message_id, std::string agent_id,
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
                                     std::string agent_id, std::string task_id,
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

std::vector<std::uint8_t> EncodeEventFrame(const proto::AgentEvent& event) {
  std::string bytes;
  REQUIRE(event.SerializeToString(&bytes));
  return zfleet::transport::EncodeFrame(std::span<const std::uint8_t>{
      reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()});
}

void PostHttp2Events(std::uint16_t port, std::vector<std::uint8_t> body) {
  asio::io_context io_context;
  tcp::resolver resolver(io_context);
  ClientContext context(io_context);
  const auto endpoints = resolver.resolve("127.0.0.1", std::to_string(port));
  asio::connect(context.socket, endpoints);

  ClientCallbacks callbacks;
  ClientSession session(callbacks, &context);
  ClientBody request_body{.bytes = std::move(body), .offset = 0};
  nghttp2_data_provider provider;
  provider.source.ptr = &request_body;
  provider.read_callback = ClientReadCallback;

  std::array<nghttp2_nv, 5> headers{
      ZFLEET_TEST_NGHTTP2_NV(":method", "POST"),
      ZFLEET_TEST_NGHTTP2_NV(":scheme", "http"),
      ZFLEET_TEST_NGHTTP2_NV(":authority", "127.0.0.1"),
      ZFLEET_TEST_NGHTTP2_NV(":path", "/v1/control/events"),
      ZFLEET_TEST_NGHTTP2_NV("content-type", "application/x-protobuf"),
  };
  CheckNghttp2(nghttp2_submit_request(session.session, nullptr, headers.data(),
                                      headers.size(), &provider, nullptr),
               "submit event request");
  CheckNghttp2(nghttp2_session_send(session.session), "send event request");

  std::array<std::uint8_t, 16 * 1024> buffer{};
  boost::system::error_code ec;
  while (!context.response_done) {
    const auto bytes_read =
        context.socket.read_some(boost::asio::buffer(buffer), ec);
    if (ec) {
      throw boost::system::system_error(ec);
    }
    const auto rv =
        nghttp2_session_mem_recv(session.session, buffer.data(), bytes_read);
    CheckNghttp2(static_cast<int>(rv), "receive response bytes");
    CheckNghttp2(nghttp2_session_send(session.session), "send pending bytes");
  }

  REQUIRE(context.response_status == "200");
}

std::vector<std::uint8_t> ExchangeHttp2EventsAndCommand(
    std::uint16_t port, std::vector<std::uint8_t> event_body,
    const std::function<void()>& after_command_stream_opened = {},
    std::size_t expected_command_frames = 1) {
  asio::io_context io_context;
  tcp::resolver resolver(io_context);
  ClientContext context(io_context);
  const auto endpoints = resolver.resolve("127.0.0.1", std::to_string(port));
  asio::connect(context.socket, endpoints);

  ClientCallbacks callbacks;
  ClientSession session(callbacks, &context);
  ClientBody request_body{.bytes = std::move(event_body), .offset = 0};
  nghttp2_data_provider provider;
  provider.source.ptr = &request_body;
  provider.read_callback = ClientReadCallback;

  std::array<nghttp2_nv, 5> event_headers{
      ZFLEET_TEST_NGHTTP2_NV(":method", "POST"),
      ZFLEET_TEST_NGHTTP2_NV(":scheme", "http"),
      ZFLEET_TEST_NGHTTP2_NV(":authority", "127.0.0.1"),
      ZFLEET_TEST_NGHTTP2_NV(":path", "/v1/control/events"),
      ZFLEET_TEST_NGHTTP2_NV("content-type", "application/x-protobuf"),
  };
  CheckNghttp2(
      nghttp2_submit_request(session.session, nullptr, event_headers.data(),
                             event_headers.size(), &provider, nullptr),
      "submit event request");
  CheckNghttp2(nghttp2_session_send(session.session), "send event request");
  while (!context.response_done) {
    std::array<std::uint8_t, 16 * 1024> buffer{};
    boost::system::error_code ec;
    const auto bytes_read =
        context.socket.read_some(boost::asio::buffer(buffer), ec);
    if (ec) {
      throw boost::system::system_error(ec);
    }
    const auto rv =
        nghttp2_session_mem_recv(session.session, buffer.data(), bytes_read);
    CheckNghttp2(static_cast<int>(rv), "receive response bytes");
    CheckNghttp2(nghttp2_session_send(session.session), "send pending bytes");
  }
  REQUIRE(context.response_status == "200");

  context.response_status.clear();
  context.response_body.clear();
  context.response_done = false;

  std::array<nghttp2_nv, 6> command_headers{
      ZFLEET_TEST_NGHTTP2_NV(":method", "GET"),
      ZFLEET_TEST_NGHTTP2_NV(":scheme", "http"),
      ZFLEET_TEST_NGHTTP2_NV(":authority", "127.0.0.1"),
      ZFLEET_TEST_NGHTTP2_NV(":path", "/v1/control/commands"),
      ZFLEET_TEST_NGHTTP2_NV("accept", "application/x-protobuf"),
      ZFLEET_TEST_NGHTTP2_NV("x-zfleet-correlation-id", "cmd-correlation-1"),
  };
  CheckNghttp2(
      nghttp2_submit_request(session.session, nullptr, command_headers.data(),
                             command_headers.size(), nullptr, nullptr),
      "submit command request");
  CheckNghttp2(nghttp2_session_send(session.session), "send command request");
  if (after_command_stream_opened) {
    after_command_stream_opened();
  }
  std::array<std::uint8_t, 16 * 1024> buffer{};
  boost::system::error_code ec;
  while (CountCompleteFrames(context.response_body) < expected_command_frames) {
    const auto bytes_read =
        context.socket.read_some(boost::asio::buffer(buffer), ec);
    if (ec) {
      throw boost::system::system_error(ec);
    }
    const auto rv =
        nghttp2_session_mem_recv(session.session, buffer.data(), bytes_read);
    CheckNghttp2(static_cast<int>(rv), "receive command bytes");
    CheckNghttp2(nghttp2_session_send(session.session), "send pending bytes");
  }
  REQUIRE(context.response_status == "200");
  return context.response_body;
}

TEST_CASE("control dispatcher persists register and heartbeat end to end") {
  const zfleet::test::ScopedTestDir test_dir("integration");
  const auto database_path = test_dir.path() / "zfleet.db";

  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  zfleet::server::ControlService service(&database);
  zfleet::server::ControlConnectionRegistry registry;
  registry.OpenConnection("conn-1", "2026-05-21T12:00:00Z");
  zfleet::server::ControlDispatcher dispatcher(&service, &registry, "conn-1");

  const auto registration_frame = EncodeEventFrame(RegisterEvent(
      "register-integration-1", "agent-integration-1", "2026-05-21T12:00:00Z"));
  const auto heartbeat_frame = EncodeEventFrame(
      HeartbeatEvent("heartbeat-integration-1", "agent-integration-1",
                     "2026-05-21T12:00:05Z"));

  std::vector<std::uint8_t> stream_bytes;
  stream_bytes.insert(stream_bytes.end(), registration_frame.begin(),
                      registration_frame.end());
  stream_bytes.insert(stream_bytes.end(), heartbeat_frame.begin(),
                      heartbeat_frame.end());

  const auto results = dispatcher.PushEventBytes(stream_bytes);

  REQUIRE(results.size() == 2);
  REQUIRE(results[0].status == zfleet::server::ControlEventStatus::kAccepted);
  REQUIRE(results[1].status == zfleet::server::ControlEventStatus::kAccepted);
  REQUIRE(CountRows(database_path, "agents") == 1);
  REQUIRE(CountRows(database_path, "audit_events") == 1);
  REQUIRE(ReadSingleTextColumn(database_path,
                               "select agent_id from agents where agent_id = ?",
                               "agent-integration-1") == "agent-integration-1");
  REQUIRE(ReadSingleTextColumn(
              database_path,
              "select last_online_at from agents where agent_id = ?",
              "agent-integration-1") == "2026-05-21T12:00:00Z");

  const auto connection = registry.FindByAgent("agent-integration-1");
  REQUIRE(connection.has_value());
  REQUIRE(connection->connection_id == "conn-1");
  REQUIRE(connection->last_heartbeat_at == "2026-05-21T12:00:05Z");
}

TEST_CASE("control server accepts h2c framed register and heartbeat") {
  const zfleet::test::ScopedTestDir test_dir("integration");
  const auto database_path = test_dir.path() / "zfleet-h2c.db";

  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  zfleet::server::ControlService service(&database);
  zfleet::server::ControlConnectionRegistry registry;
  zfleet::server::ControlServer server("127.0.0.1:0", &database, &service,
                                       &registry);

  std::thread server_thread([&server]() { server.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  const auto registration_frame = EncodeEventFrame(
      RegisterEvent("register-h2c-1", "agent-h2c-1", "2026-05-21T13:00:00Z"));
  const auto heartbeat_frame = EncodeEventFrame(
      HeartbeatEvent("heartbeat-h2c-1", "agent-h2c-1", "2026-05-21T13:00:05Z"));
  std::vector<std::uint8_t> request_body;
  request_body.insert(request_body.end(), registration_frame.begin(),
                      registration_frame.end());
  request_body.insert(request_body.end(), heartbeat_frame.begin(),
                      heartbeat_frame.end());

  PostHttp2Events(server.port(), std::move(request_body));

  server.Stop();
  if (server_thread.joinable()) {
    server_thread.join();
  }

  REQUIRE(CountRows(database_path, "agents") == 1);
  REQUIRE(ReadSingleTextColumn(database_path,
                               "select agent_id from agents where agent_id = ?",
                               "agent-h2c-1") == "agent-h2c-1");
  REQUIRE(ReadSingleTextColumn(
              database_path,
              "select last_online_at from agents where agent_id = ?",
              "agent-h2c-1") == "2026-05-21T13:00:00Z");
}

TEST_CASE("control client exposes rejected command stream status") {
  const zfleet::test::ScopedTestDir test_dir("integration");
  const auto database_path = test_dir.path() / "zfleet-h2c-command-reject.db";

  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  zfleet::server::ControlService service(&database);
  zfleet::server::ControlConnectionRegistry registry;
  zfleet::server::ControlServer server("127.0.0.1:0", &database, &service,
                                       &registry);

  std::thread server_thread([&server]() { server.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  zfleet::agent::Http2ControlClient client(std::string("http://127.0.0.1:") +
                                           std::to_string(server.port()));
  const auto status = client.StartCommandStream("reject-correlation-1");
  REQUIRE(status == "404");
  REQUIRE_FALSE(client.command_stream_open());
  const auto close_error = client.command_stream_error_code();
  REQUIRE((!close_error.has_value() || *close_error == NGHTTP2_NO_ERROR));

  client.Close();
  server.Stop();
  if (server_thread.joinable()) {
    server_thread.join();
  }
}

TEST_CASE("control server rejects connections over configured limit") {
  const zfleet::test::ScopedTestDir test_dir("integration");
  const auto database_path = test_dir.path() / "zfleet-h2c-overload.db";

  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  zfleet::server::ControlService service(&database);
  zfleet::server::ControlConnectionRegistry registry;
  zfleet::server::ControlServer server(
      "127.0.0.1:0", &database, &service, &registry,
      zfleet::server::ControlServerOptions{.max_connections = 1});

  std::thread server_thread([&server]() { server.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  asio::io_context io_context;
  tcp::endpoint endpoint(asio::ip::make_address("127.0.0.1"), server.port());
  tcp::socket first_socket(io_context);
  first_socket.connect(endpoint);
  for (int attempt = 0; attempt < 50 && registry.ActiveConnectionCount() == 0;
       ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  REQUIRE(registry.ActiveConnectionCount() == 1);

  tcp::socket second_socket(io_context);
  second_socket.connect(endpoint);
  second_socket.non_blocking(true);

  bool rejected = false;
  std::array<char, 1> buffer{};
  for (int attempt = 0; attempt < 50; ++attempt) {
    boost::system::error_code ec;
    second_socket.read_some(asio::buffer(buffer), ec);
    if (ec == asio::error::eof || ec == asio::error::connection_reset ||
        ec == asio::error::connection_aborted) {
      rejected = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  boost::system::error_code ignored;
  first_socket.close(ignored);
  second_socket.close(ignored);
  server.Stop();
  if (server_thread.joinable()) {
    server_thread.join();
  }

  REQUIRE(rejected);
}

}  // namespace
