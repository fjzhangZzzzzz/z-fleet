#include "runtime.h"
#include "state.h"

#include "database.h"
#include "control_connection_registry.h"
#include "control_server.h"
#include "control_service.h"

#include "test_util.h"

#include <SQLiteCpp/SQLiteCpp.h>
#include "zfleet/protocol/v1/agent_control.pb.h"
#include "zfleet/transport/frame_codec.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/asio/write.hpp>
#include <catch2/catch_test_macros.hpp>
#include <nghttp2/nghttp2.h>

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

#define ZFLEET_TEST_NGHTTP2_NV(NAME, VALUE)                        \
  nghttp2_nv {                                                     \
    reinterpret_cast<std::uint8_t*>(const_cast<char*>(NAME)),      \
        reinterpret_cast<std::uint8_t*>(const_cast<char*>(VALUE)), \
        sizeof(NAME) - 1, sizeof(VALUE) - 1, NGHTTP2_NV_FLAG_NONE  \
  }

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

int CountRows(const std::filesystem::path& database_path,
              const std::string& table_name) {
  SQLite::Database db(database_path.string(), SQLite::OPEN_READONLY);
  db.exec("PRAGMA busy_timeout=5000");
  SQLite::Statement query(db, "select count(*) from " + table_name);
  query.executeStep();
  return query.getColumn(0).getInt();
}

int CountByQuery(const std::filesystem::path& database_path,
                 const std::string& query_text) {
  SQLite::Database db(database_path.string(), SQLite::OPEN_READONLY);
  db.exec("PRAGMA busy_timeout=5000");
  SQLite::Statement query(db, query_text);
  query.executeStep();
  return query.getColumn(0).getInt();
}

std::string ReadSingleTextColumn(const std::filesystem::path& database_path,
                                 const std::string& query_text,
                                 const std::string& parameter) {
  SQLite::Database db(database_path.string(), SQLite::OPEN_READONLY);
  db.exec("PRAGMA busy_timeout=5000");
  SQLite::Statement query(db, query_text);
  query.bind(1, parameter);
  if (!query.executeStep()) {
    return {};
  }
  return query.getColumn(0).getString();
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

bool WaitForTaskState(const std::filesystem::path& database_path,
                      const std::string& task_id,
                      const std::string& expected_state) {
  for (int attempt = 0; attempt < 100; ++attempt) {
    if (ReadSingleTextColumn(database_path,
                             "select state from tasks where task_id = ?",
                             task_id) == expected_state) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return ReadSingleTextColumn(database_path,
                              "select state from tasks where task_id = ?",
                              task_id) == expected_state;
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
    const auto rv =
        nghttp2_session_mem_recv(session, buffer.data(), bytes_read);
    CheckNghttp2(static_cast<int>(rv), "receive response bytes");
    CheckNghttp2(nghttp2_session_send(session), "send pending bytes");
  }
}

TEST_CASE("agent runtime completes task over control channel") {
  const zfleet::test::ScopedTestDir test_dir("integration");
  const auto test_root = test_dir.path();
  const auto database_path = test_root / "zfleet-agent-runtime.db";

  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  zfleet::server::ControlService service(&database);
  zfleet::server::ControlConnectionRegistry registry;
  zfleet::server::ControlServer server("127.0.0.1:0", &database, &service,
                                       &registry);

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
    zfleet::server::ControlServer& server;
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

  REQUIRE(WaitForTaskState(database_path, "task-runtime-1", "succeeded"));

  if (runtime_error != nullptr) {
    std::rethrow_exception(runtime_error);
  }
  if (server_error != nullptr) {
    std::rethrow_exception(server_error);
  }

  REQUIRE(ReadSingleTextColumn(database_path,
                               "select agent_id from agents where agent_id = ?",
                               agent_state.agent_id) == agent_state.agent_id);
  REQUIRE_FALSE(ReadSingleTextColumn(
                    database_path,
                    "select last_online_at from agents where agent_id = ?",
                    agent_state.agent_id)
                    .empty());
  REQUIRE(CountRows(database_path, "task_results") == 1);
  REQUIRE(ReadSingleTextColumn(database_path,
                               "select state from tasks where task_id = ?",
                               "task-runtime-1") == "succeeded");
}

TEST_CASE("agent runtime reconnects and registers again after server restart") {
  const zfleet::test::ScopedTestDir test_dir("integration");
  const auto test_root = test_dir.path();
  const auto database_path = test_root / "zfleet-agent-reconnect.db";

  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  zfleet::server::ControlService service(&database);
  zfleet::server::ControlConnectionRegistry first_registry;
  zfleet::server::ControlServer first_server("127.0.0.1:0", &database, &service,
                                             &first_registry);

  std::thread first_server_thread([&first_server]() { first_server.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  const auto port = first_server.port();

  zfleet::agent::AgentConfig config{
      .control_url = std::string("http://127.0.0.1:") + std::to_string(port),
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

  zfleet::server::ControlConnectionRegistry second_registry;
  zfleet::server::ControlServer second_server(
      std::string("127.0.0.1:") + std::to_string(port), &database, &service,
      &second_registry);
  std::thread second_server_thread([&second_server]() { second_server.Run(); });

  struct ServerCleanup {
    zfleet::server::ControlServer& server;
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
  REQUIRE(CountByQuery(database_path,
                       "select count(*) from audit_events where event_type = "
                       "'agent.register'") >= 2);

  if (runtime_error != nullptr) {
    std::rethrow_exception(runtime_error);
  }
}

}  // namespace
