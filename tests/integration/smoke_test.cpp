#include "runtime.h"
#include "state.h"

#include "database.h"
#include "http2_control_client.h"
#include "http2_connection_registry.h"
#include "http2_control_dispatcher.h"
#include "http2_control_server.h"
#include "http2_control_service.h"

#include "test_util.h"

#include "zfleet/core/time.h"
#include "zfleet/core/version.h"
#include "zfleet/platform/system.h"
#include "zfleet/protocol/message.h"
#include "zfleet/protocol/v1/agent_control.pb.h"
#include "zfleet/transport/frame_codec.h"

#include <SQLiteCpp/SQLiteCpp.h>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/write.hpp>
#include <catch2/catch_test_macros.hpp>
#include <nghttp2/nghttp2.h>
#include <sqlite3.h>

#include <algorithm>
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

#define ZFLEET_TEST_NGHTTP2_NV(NAME, VALUE)                                 \
  nghttp2_nv {                                                              \
    reinterpret_cast<std::uint8_t*>(const_cast<char*>(NAME)),               \
        reinterpret_cast<std::uint8_t*>(const_cast<char*>(VALUE)),          \
        sizeof(NAME) - 1, sizeof(VALUE) - 1, NGHTTP2_NV_FLAG_NONE           \
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

void CheckNghttp2(int rv, std::string_view operation) {
  if (rv < 0) {
    throw std::runtime_error(std::string(operation) + " failed: " +
                             nghttp2_strerror(rv));
  }
}

ssize_t ClientSendCallback(nghttp2_session* /*session*/,
                           const std::uint8_t* data,
                           std::size_t length,
                           int /*flags*/,
                           void* user_data) {
  auto* context = static_cast<ClientContext*>(user_data);
  boost::asio::write(context->socket, boost::asio::buffer(data, length));
  return static_cast<ssize_t>(length);
}

ssize_t ClientReadCallback(nghttp2_session* /*session*/,
                           std::int32_t /*stream_id*/,
                           std::uint8_t* buffer,
                           std::size_t length,
                           std::uint32_t* data_flags,
                           nghttp2_data_source* source,
                           void* /*user_data*/) {
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
                         const nghttp2_frame* frame,
                         const std::uint8_t* name,
                         std::size_t name_length,
                         const std::uint8_t* value,
                         std::size_t value_length,
                         std::uint8_t /*flags*/,
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
                            const nghttp2_frame* frame,
                            void* user_data) {
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
                                const std::uint8_t* data,
                                std::size_t length,
                                void* user_data) {
  auto* context = static_cast<ClientContext*>(user_data);
  context->response_body.insert(context->response_body.end(), data,
                                data + length);
  return 0;
}

struct ClientCallbacks {
  nghttp2_session_callbacks* callbacks = nullptr;

  ClientCallbacks() {
    CheckNghttp2(nghttp2_session_callbacks_new(&callbacks),
                 "create client callbacks");
    nghttp2_session_callbacks_set_send_callback(callbacks,
                                                ClientSendCallback);
    nghttp2_session_callbacks_set_on_header_callback(callbacks,
                                                     ClientHeaderCallback);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(
        callbacks, ClientDataChunkRecvCallback);
    nghttp2_session_callbacks_set_on_frame_recv_callback(
        callbacks, ClientFrameRecvCallback);
  }

  ~ClientCallbacks() {
    nghttp2_session_callbacks_del(callbacks);
  }
};

struct ClientSession {
  nghttp2_session* session = nullptr;

  ClientSession(const ClientCallbacks& callbacks, ClientContext* context) {
    CheckNghttp2(
        nghttp2_session_client_new(&session, callbacks.callbacks, context),
        "create client session");
    CheckNghttp2(nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, nullptr, 0),
                 "submit client settings");
  }

  ~ClientSession() {
    nghttp2_session_del(session);
  }
};

SQLite::Database OpenTestDatabase(const std::filesystem::path& database_path,
                                  const int flags) {
  SQLite::Database db(database_path.string(), flags);
  db.exec("PRAGMA busy_timeout=5000");
  return db;
}

bool IsRecoverableBusy(const SQLite::Exception& ex) {
  return ex.getErrorCode() == SQLITE_BUSY ||
         ex.getErrorCode() == SQLITE_LOCKED;
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

std::string ReadTaskState(const std::filesystem::path& database_path,
                          const std::string& task_id) {
  return ReadSingleTextColumn(database_path,
                              "select state from tasks where task_id = ?",
                              task_id);
}

bool WaitForTaskState(const std::filesystem::path& database_path,
                      const std::string& task_id,
                      const std::string& expected_state) {
  for (int attempt = 0; attempt < 100; ++attempt) {
    if (ReadTaskState(database_path, task_id) == expected_state) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return ReadTaskState(database_path, task_id) == expected_state;
}

bool WaitForSingleTextColumn(const std::filesystem::path& database_path,
                             const std::string& query_text,
                             const std::string& parameter,
                             const std::string& expected_value) {
  for (int attempt = 0; attempt < 100; ++attempt) {
    if (ReadSingleTextColumn(database_path, query_text, parameter) ==
        expected_value) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return ReadSingleTextColumn(database_path, query_text, parameter) ==
         expected_value;
}

proto::AgentEvent RegisterEvent(std::string message_id,
                                std::string agent_id,
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

void ReceiveUntilResponseDone(ClientContext* context,
                              nghttp2_session* session) {
  std::array<std::uint8_t, 16 * 1024> buffer{};
  boost::system::error_code ec;
  while (!context->response_done) {
    const auto bytes_read =
        context->socket.read_some(boost::asio::buffer(buffer), ec);
    if (ec) {
      throw boost::system::system_error(ec);
    }
    const auto rv = nghttp2_session_mem_recv(session, buffer.data(),
                                             bytes_read);
    CheckNghttp2(static_cast<int>(rv), "receive response bytes");
    CheckNghttp2(nghttp2_session_send(session), "send pending bytes");
  }
}

std::size_t CountCompleteFrames(const std::vector<std::uint8_t>& bytes) {
  std::size_t offset = 0;
  std::size_t count = 0;
  while (bytes.size() >= offset + 4) {
    const auto length =
        (static_cast<std::uint32_t>(bytes[offset]) << 24) |
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

void ReceiveUntilCommandFrames(ClientContext* context,
                               nghttp2_session* session,
                               std::size_t expected_frames) {
  std::array<std::uint8_t, 16 * 1024> buffer{};
  boost::system::error_code ec;
  while (CountCompleteFrames(context->response_body) < expected_frames) {
    const auto bytes_read =
        context->socket.read_some(boost::asio::buffer(buffer), ec);
    if (ec) {
      throw boost::system::system_error(ec);
    }
    const auto rv = nghttp2_session_mem_recv(session, buffer.data(),
                                             bytes_read);
    CheckNghttp2(static_cast<int>(rv), "receive command bytes");
    CheckNghttp2(nghttp2_session_send(session), "send pending bytes");
  }
}

std::vector<std::uint8_t> ExchangeHttp2EventsAndCommand(
    std::uint16_t port,
    std::vector<std::uint8_t> event_body,
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
  CheckNghttp2(nghttp2_submit_request(session.session, nullptr,
                                      event_headers.data(),
                                      event_headers.size(), &provider,
                                      nullptr),
               "submit event request");
  CheckNghttp2(nghttp2_session_send(session.session), "send event request");
  ReceiveUntilResponseDone(&context, session.session);
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
  CheckNghttp2(nghttp2_submit_request(session.session, nullptr,
                                      command_headers.data(),
                                      command_headers.size(), nullptr,
                                      nullptr),
               "submit command request");
  CheckNghttp2(nghttp2_session_send(session.session), "send command request");
  if (after_command_stream_opened) {
    after_command_stream_opened();
  }
  ReceiveUntilCommandFrames(&context, session.session, expected_command_frames);
  REQUIRE(context.response_status == "200");
  return context.response_body;
}

} // namespace

TEST_CASE("integration scaffold links core platform and protocol modules") {
  REQUIRE_FALSE(zfleet::core::project_name().empty());
  REQUIRE_FALSE(zfleet::core::version().empty());
  REQUIRE_FALSE(zfleet::platform::os_name().empty());
  REQUIRE_FALSE(zfleet::protocol::protocol_version().empty());
  REQUIRE_FALSE(zfleet::core::NowUtcRfc3339().empty());
}

TEST_CASE("http2 control dispatcher persists register and heartbeat end to end") {
  const zfleet::test::ScopedTestDir test_dir("integration");
  const auto database_path = test_dir.path() / "zfleet.db";

  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  zfleet::server::Http2ControlService service(&database);
  zfleet::server::Http2ConnectionRegistry registry;
  registry.OpenConnection("conn-1", "2026-05-21T12:00:00Z");
  zfleet::server::Http2ControlDispatcher dispatcher(
      &service, &registry, "conn-1");

  const auto registration_frame = EncodeEventFrame(RegisterEvent(
      "register-integration-1", "agent-integration-1",
      "2026-05-21T12:00:00Z"));
  const auto heartbeat_frame = EncodeEventFrame(HeartbeatEvent(
      "heartbeat-integration-1", "agent-integration-1",
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
  REQUIRE(ReadSingleTextColumn(
              database_path,
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

TEST_CASE("http2 control server accepts h2c framed register and heartbeat") {
  const zfleet::test::ScopedTestDir test_dir("integration");
  const auto database_path = test_dir.path() / "zfleet-h2c.db";

  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  zfleet::server::Http2ControlService service(&database);
  zfleet::server::Http2ConnectionRegistry registry;
  zfleet::server::Http2ControlServer server("127.0.0.1:0", &database, &service,
                                            &registry);

  std::thread server_thread([&server]() { server.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  const auto registration_frame = EncodeEventFrame(RegisterEvent(
      "register-h2c-1", "agent-h2c-1", "2026-05-21T13:00:00Z"));
  const auto heartbeat_frame = EncodeEventFrame(HeartbeatEvent(
      "heartbeat-h2c-1", "agent-h2c-1", "2026-05-21T13:00:05Z"));
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
  REQUIRE(ReadSingleTextColumn(
              database_path,
              "select agent_id from agents where agent_id = ?",
              "agent-h2c-1") == "agent-h2c-1");
  REQUIRE(ReadSingleTextColumn(
              database_path,
              "select last_online_at from agents where agent_id = ?",
              "agent-h2c-1") == "2026-05-21T13:00:00Z");
}

TEST_CASE("http2 control client exposes rejected command stream status") {
  const zfleet::test::ScopedTestDir test_dir("integration");
  const auto database_path = test_dir.path() / "zfleet-h2c-command-reject.db";

  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  zfleet::server::Http2ControlService service(&database);
  zfleet::server::Http2ConnectionRegistry registry;
  zfleet::server::Http2ControlServer server("127.0.0.1:0", &database, &service,
                                            &registry);

  std::thread server_thread([&server]() { server.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  zfleet::agent::Http2ControlClient client(
      std::string("http://127.0.0.1:") + std::to_string(server.port()));
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

TEST_CASE("http2 control server sends assigned task on command stream") {
  const zfleet::test::ScopedTestDir test_dir("integration");
  const auto database_path = test_dir.path() / "zfleet-h2c-command.db";

  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  database.UpsertAgent(zfleet::protocol::AgentRegistration{
      .protocol_version = "v1",
      .request_id = "seed-agent",
      .agent_id = "agent-command-1",
      .occurred_at = "2026-05-21T13:59:00Z",
      .agent_version = "0.1.0",
      .hostname = "devbox-01",
      .os = "linux",
      .arch = "x86_64",
  });
  zfleet::server::Http2ControlService service(&database);
  zfleet::server::Http2ConnectionRegistry registry;
  zfleet::server::Http2ControlServer server("127.0.0.1:0", &database,
                                            &service, &registry);

  std::thread server_thread([&server]() { server.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  const auto registration_frame = EncodeEventFrame(RegisterEvent(
      "register-command-1", "agent-command-1", "2026-05-21T14:00:01Z"));
  const auto heartbeat_frame = EncodeEventFrame(HeartbeatEvent(
      "heartbeat-command-1", "agent-command-1", "2026-05-21T14:00:02Z"));
  std::vector<std::uint8_t> request_body;
  request_body.insert(request_body.end(), registration_frame.begin(),
                      registration_frame.end());
  request_body.insert(request_body.end(), heartbeat_frame.begin(),
                      heartbeat_frame.end());

  const auto command_body = ExchangeHttp2EventsAndCommand(
      server.port(), std::move(request_body), [&database]() {
        database.EnqueueTask(zfleet::protocol::Task{
            .protocol_version = "v1",
            .task_id = "task-command-1",
            .agent_id = "agent-command-1",
            .task_type = zfleet::protocol::TaskType::collect_basic_inventory,
            .capability_level = zfleet::protocol::CapabilityLevel::readonly,
            .created_at = "2026-05-21T14:00:00Z",
            .expires_at = "2099-05-21T14:05:00Z",
            .input = zfleet::protocol::CollectBasicInventoryInput{},
        });
      });

  const auto running_frame = EncodeEventFrame(TaskRunningEvent(
      "running-command-1", "agent-command-1", "task-command-1",
      "2026-05-21T14:00:03Z"));
  const auto result_frame = EncodeEventFrame(TaskSucceededEvent(
      "result-command-1", "agent-command-1", "task-command-1",
      "2026-05-21T14:00:04Z"));
  std::vector<std::uint8_t> result_body;
  result_body.insert(result_body.end(), running_frame.begin(),
                     running_frame.end());
  result_body.insert(result_body.end(), result_frame.begin(),
                     result_frame.end());
  PostHttp2Events(server.port(), std::move(result_body));

  server.Stop();
  if (server_thread.joinable()) {
    server_thread.join();
  }

  zfleet::transport::FrameDecoder decoder;
  const auto frames = decoder.Push(command_body);
  REQUIRE(frames.size() == 1);
  proto::ServerCommand command;
  REQUIRE(command.ParseFromArray(frames[0].data(),
                                 static_cast<int>(frames[0].size())));
  REQUIRE(command.agent_id() == "agent-command-1");
  REQUIRE(command.correlation_id() == "cmd-correlation-1");
  REQUIRE(command.payload_case() == proto::ServerCommand::kTaskAssigned);
  REQUIRE(command.task_assigned().task_id() == "task-command-1");
  REQUIRE(command.task_assigned().task_type() ==
          proto::TASK_TYPE_COLLECT_BASIC_INVENTORY);
  REQUIRE(command.task_assigned().capability_level() ==
          proto::CAPABILITY_LEVEL_READONLY);
  REQUIRE(ReadSingleTextColumn(database_path,
                               "select state from tasks where task_id = ?",
                               "task-command-1") == "succeeded");
  REQUIRE(CountRows(database_path, "task_results") == 1);
}

TEST_CASE("http2 control server sends multiple queued tasks on one command stream") {
  const zfleet::test::ScopedTestDir test_dir("integration");
  const auto database_path = test_dir.path() / "zfleet-h2c-command-multi.db";

  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  database.UpsertAgent(zfleet::protocol::AgentRegistration{
      .protocol_version = "v1",
      .request_id = "seed-agent-multi",
      .agent_id = "agent-command-multi",
      .occurred_at = "2026-05-21T13:59:00Z",
      .agent_version = "0.1.0",
      .hostname = "devbox-01",
      .os = "linux",
      .arch = "x86_64",
  });

  zfleet::server::Http2ControlService service(&database);
  zfleet::server::Http2ConnectionRegistry registry;
  zfleet::server::Http2ControlServer server("127.0.0.1:0", &database,
                                            &service, &registry);

  std::thread server_thread([&server]() { server.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  const auto registration_frame = EncodeEventFrame(RegisterEvent(
      "register-command-multi", "agent-command-multi",
      "2026-05-21T14:00:01Z"));
  const auto heartbeat_frame = EncodeEventFrame(HeartbeatEvent(
      "heartbeat-command-multi", "agent-command-multi",
      "2026-05-21T14:00:02Z"));
  std::vector<std::uint8_t> request_body;
  request_body.insert(request_body.end(), registration_frame.begin(),
                      registration_frame.end());
  request_body.insert(request_body.end(), heartbeat_frame.begin(),
                      heartbeat_frame.end());

  const auto command_body = ExchangeHttp2EventsAndCommand(
      server.port(), std::move(request_body), [&database]() {
        for (int index = 1; index <= 2; ++index) {
          database.EnqueueTask(zfleet::protocol::Task{
              .protocol_version = "v1",
              .task_id = "task-command-multi-" + std::to_string(index),
              .agent_id = "agent-command-multi",
              .task_type = zfleet::protocol::TaskType::collect_basic_inventory,
              .capability_level = zfleet::protocol::CapabilityLevel::readonly,
              .created_at = "2026-05-21T14:00:00Z",
              .expires_at = "2099-05-21T14:05:00Z",
              .input = zfleet::protocol::CollectBasicInventoryInput{},
          });
        }
      }, 2);

  server.Stop();
  if (server_thread.joinable()) {
    server_thread.join();
  }

  zfleet::transport::FrameDecoder decoder;
  const auto frames = decoder.Push(command_body);
  REQUIRE(frames.size() == 2);
  for (std::size_t index = 0; index < frames.size(); ++index) {
    proto::ServerCommand command;
    REQUIRE(command.ParseFromArray(frames[index].data(),
                                   static_cast<int>(frames[index].size())));
    REQUIRE(command.agent_id() == "agent-command-multi");
    REQUIRE(command.payload_case() == proto::ServerCommand::kTaskAssigned);
    REQUIRE(command.task_assigned().task_id() ==
            "task-command-multi-" + std::to_string(index + 1));
  }
  REQUIRE(ReadSingleTextColumn(database_path,
                               "select state from tasks where task_id = ?",
                               "task-command-multi-1") == "assigned");
  REQUIRE(ReadSingleTextColumn(database_path,
                               "select state from tasks where task_id = ?",
                               "task-command-multi-2") == "assigned");
}

TEST_CASE("agent runtime completes task over http2 control channel") {
  const zfleet::test::ScopedTestDir test_dir("integration");
  const auto test_root = test_dir.path();
  const auto database_path = test_root / "zfleet-agent-runtime.db";

  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  zfleet::server::Http2ControlService service(&database);
  zfleet::server::Http2ConnectionRegistry registry;
  zfleet::server::Http2ControlServer server("127.0.0.1:0", &database,
                                            &service, &registry);

  std::exception_ptr server_error;
  std::thread server_thread([&server, &server_error]() {
    try {
      server.Run();
    } catch (...) {
      server_error = std::current_exception();
    }
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  zfleet::agent::AgentConfig config{
      .control_url =
          std::string("http://127.0.0.1:") + std::to_string(server.port()),
      .data_dir = test_root / "agent-data",
      .state_path = "agent-state.toml",
      .heartbeat_interval_seconds = 1,
      .reconnect_initial_delay_seconds = 1,
      .reconnect_max_delay_seconds = 2,
  };
  const auto state_path = zfleet::agent::StatePathFor(config);
  const auto agent_state = zfleet::agent::LoadOrCreateState(state_path);

  database.EnqueueTask(zfleet::protocol::Task{
      .protocol_version = "v1",
      .task_id = "task-runtime-1",
      .agent_id = agent_state.agent_id,
      .task_type = zfleet::protocol::TaskType::collect_basic_inventory,
      .capability_level = zfleet::protocol::CapabilityLevel::readonly,
      .created_at = "2026-05-21T15:00:00Z",
      .expires_at = "2099-05-21T15:05:00Z",
      .input = zfleet::protocol::CollectBasicInventoryInput{},
  });

  zfleet::agent::AgentRuntime runtime(config);
  std::exception_ptr runtime_error;
  std::thread runtime_thread([&runtime, &runtime_error]() {
    try {
      runtime.Run();
    } catch (...) {
      runtime_error = std::current_exception();
    }
  });

  struct ThreadCleanup {
    zfleet::agent::AgentRuntime& runtime;
    std::thread& runtime_thread;
    zfleet::server::Http2ControlServer& server;
    std::thread& server_thread;

    ~ThreadCleanup() {
      runtime.RequestStop();
      if (runtime_thread.joinable()) {
        runtime_thread.join();
      }
      server.Stop();
      if (server_thread.joinable()) {
        server_thread.join();
      }
    }
  } thread_cleanup{runtime, runtime_thread, server, server_thread};

  const auto task_completed =
      WaitForTaskState(database_path, "task-runtime-1", "succeeded");

  if (runtime_error != nullptr) {
    std::rethrow_exception(runtime_error);
  }
  if (server_error != nullptr) {
    std::rethrow_exception(server_error);
  }

  REQUIRE(ReadSingleTextColumn(
              database_path,
              "select agent_id from agents where agent_id = ?",
              agent_state.agent_id) == agent_state.agent_id);
  REQUIRE_FALSE(ReadSingleTextColumn(
                    database_path,
                    "select last_online_at from agents where agent_id = ?",
                    agent_state.agent_id)
                    .empty());
  REQUIRE(CountRows(database_path, "task_results") == 1);
  REQUIRE(task_completed);
  REQUIRE(ReadTaskState(database_path, "task-runtime-1") == "succeeded");
}

TEST_CASE("agent runtime reconnects and registers again after server restart") {
  const zfleet::test::ScopedTestDir test_dir("integration");
  const auto test_root = test_dir.path();
  const auto database_path = test_root / "zfleet-agent-reconnect.db";

  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  zfleet::server::Http2ControlService service(&database);
  zfleet::server::Http2ConnectionRegistry first_registry;
  zfleet::server::Http2ControlServer first_server(
      "127.0.0.1:0", &database, &service, &first_registry);

  std::thread first_server_thread([&first_server]() { first_server.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  const auto port = first_server.port();

  zfleet::agent::AgentConfig config{
      .control_url =
          std::string("http://127.0.0.1:") + std::to_string(port),
      .data_dir = test_root / "agent-data",
      .state_path = "agent-state.toml",
      .heartbeat_interval_seconds = 1,
      .reconnect_initial_delay_seconds = 1,
      .reconnect_max_delay_seconds = 2,
  };
  const auto state_path = zfleet::agent::StatePathFor(config);
  const auto agent_state = zfleet::agent::LoadOrCreateState(state_path);

  zfleet::agent::AgentRuntime runtime(config);
  std::exception_ptr runtime_error;
  std::thread runtime_thread([&runtime, &runtime_error]() {
    try {
      runtime.Run();
    } catch (...) {
      runtime_error = std::current_exception();
    }
  });

  struct RuntimeCleanup {
    zfleet::agent::AgentRuntime& runtime;
    std::thread& runtime_thread;

    ~RuntimeCleanup() {
      runtime.RequestStop();
      if (runtime_thread.joinable()) {
        runtime_thread.join();
      }
    }
  } runtime_cleanup{runtime, runtime_thread};

  REQUIRE(WaitForSingleTextColumn(
      database_path, "select status from agents where agent_id = ?",
      agent_state.agent_id, "online"));

  first_server.Stop();
  if (first_server_thread.joinable()) {
    first_server_thread.join();
  }

  database.EnqueueTask(zfleet::protocol::Task{
      .protocol_version = "v1",
      .task_id = "task-reconnect-1",
      .agent_id = agent_state.agent_id,
      .task_type = zfleet::protocol::TaskType::collect_basic_inventory,
      .capability_level = zfleet::protocol::CapabilityLevel::readonly,
      .created_at = "2026-05-21T16:00:00Z",
      .expires_at = "2099-05-21T16:05:00Z",
      .input = zfleet::protocol::CollectBasicInventoryInput{},
  });

  zfleet::server::Http2ConnectionRegistry second_registry;
  zfleet::server::Http2ControlServer second_server(
      std::string("127.0.0.1:") + std::to_string(port), &database, &service,
      &second_registry);
  std::thread second_server_thread([&second_server]() { second_server.Run(); });

  struct ServerCleanup {
    zfleet::server::Http2ControlServer& server;
    std::thread& thread;

    ~ServerCleanup() {
      server.Stop();
      if (thread.joinable()) {
        thread.join();
      }
    }
  } second_server_cleanup{second_server, second_server_thread};

  REQUIRE(WaitForTaskState(database_path, "task-reconnect-1", "succeeded"));
  REQUIRE(CountRows(database_path, "task_results") == 1);
  REQUIRE(CountByQuery(
              database_path,
              "select count(*) from audit_events where event_type = "
              "'agent.register'") >= 2);

  if (runtime_error != nullptr) {
    std::rethrow_exception(runtime_error);
  }
}
