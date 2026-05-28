#include "runtime.h"
#include "state.h"

#include "database.h"
#include "control_connection_registry.h"
#include "control_server.h"
#include "control_service.h"
#include "admin_http_server.h"

#include "test_util.h"

#include "zfleet/platform/system.h"
#include "zfleet/package/manifest.h"
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

bool IsRecoverableBusy(const SQLite::Exception& ex) {
  return ex.getErrorCode() == SQLITE_BUSY || ex.getErrorCode() == SQLITE_LOCKED;
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

std::string ReadSingleTextColumn(const std::filesystem::path& database_path,
                                 const std::string& query_text,
                                 const std::string& parameter) {
  for (int attempt = 0; attempt < kSqliteReadAttempts; ++attempt) {
    try {
      SQLite::Database db(database_path.string(), SQLite::OPEN_READONLY);
      db.exec("PRAGMA busy_timeout=5000");
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
  return WaitForSingleTextColumn(database_path,
                                 "select state from tasks where task_id = ?",
                                 task_id, expected_state);
}

proto::AgentEvent RegisterEvent(std::string message_id, std::string agent_id,
                                std::string occurred_at,
                                std::string agent_version,
                                const std::string& os,
                                const std::string& arch) {
  proto::AgentEvent event;
  event.set_protocol_version("v1");
  event.set_message_id(std::move(message_id));
  event.set_agent_id(std::move(agent_id));
  event.set_occurred_at(std::move(occurred_at));
  auto* payload = event.mutable_register_();
  payload->set_agent_version(std::move(agent_version));
  payload->set_hostname("devbox-01");
  payload->set_os(os);
  payload->set_arch(arch);
  return event;
}

proto::AgentEvent TaskSucceededPackageUpdateEvent(
    std::string message_id, std::string agent_id, std::string task_id,
    std::string occurred_at, std::string package_id, std::string version) {
  proto::AgentEvent event;
  event.set_protocol_version("v1");
  event.set_message_id(std::move(message_id));
  event.set_agent_id(std::move(agent_id));
  event.set_occurred_at(std::move(occurred_at));
  auto* payload = event.mutable_task_result();
  payload->set_task_id(std::move(task_id));
  payload->set_task_type(proto::TASK_TYPE_PACKAGE_UPDATE);
  payload->set_status(proto::TASK_EXECUTION_STATUS_SUCCEEDED);
  auto* update = payload->mutable_package_update();
  update->set_component("agent");
  update->set_package_id(std::move(package_id));
  update->set_version(std::move(version));
  update->set_state("applied");
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

}  // namespace

TEST_CASE(
    "install options runtime online and upgrade request flow end to end") {
  const zfleet::test::ScopedTestDir test_dir("integration");
  const auto test_root = test_dir.path();
  const auto database_path = test_root / "zfleet-upgrade-flow.db";

  zfleet::server::ServerDatabase database(database_path);
  database.Initialize();
  zfleet::server::ControlService service(&database);
  zfleet::server::ControlConnectionRegistry registry;
  zfleet::server::ControlServer control_server("127.0.0.1:0", &database,
                                               &service, &registry);

  zfleet::server::AdminHttpServerOptions admin_options;
  admin_options.allow_high_risk_write = true;
  admin_options.web_static_root = test_root / "web";
  zfleet::test::WriteTextFile(test_root / "web" / "install.html", "install");
  zfleet::test::WriteTextFile(test_root / "web" / "index.html", "index");
  zfleet::test::WriteTextFile(test_root / "web" / "admin" / "packages.html",
                              "packages");
  zfleet::test::WriteTextFile(test_root / "web" / "agents.html", "agents");
  zfleet::test::WriteTextFile(test_root / "web" / "assets" / "admin.css",
                              "body{}");
  zfleet::test::WriteTextFile(test_root / "web" / "assets" / "admin.js",
                              "console.log('ok');");
  zfleet::test::WriteTextFile(
      test_root / "web" / "scripts" / "install" / "linux.sh",
      "#!/bin/sh\necho ok\n");
  zfleet::test::WriteTextFile(
      test_root / "web" / "scripts" / "install" / "windows.ps1",
      "Write-Output ok\n");
  zfleet::server::AdminHttpServer admin_server(
      "127.0.0.1:0", &database, test_root / "packages", test_root / "web",
      admin_options);

  const std::string platform = std::string(zfleet::platform::os_name());
  const std::string arch = zfleet::platform::architecture_name();

  zfleet::package::Manifest agent_manifest{
      .schema_version = 1,
      .component = "agent",
      .version = "0.2.0",
      .platform = platform,
      .arch = arch,
      .build_type = "release",
      .min_installer_version = "0.1.0",
      .files = {},
  };
  zfleet::package::Manifest installer_manifest{
      .schema_version = 1,
      .component = "installer",
      .version = "0.1.0",
      .platform = platform,
      .arch = arch,
      .build_type = "release",
      .min_installer_version = "0.1.0",
      .files = {},
  };

  const auto now = "2026-05-25T11:00:00Z";
  const std::string agent_package_id = "pkg-agent-0.2.0";
  const std::string installer_package_id = "pkg-installer-0.1.0";
  database.UpsertAgentPackage(zfleet::server::AgentPackageRecord{
      .package_id = agent_package_id,
      .component = "agent",
      .version = "0.2.0",
      .platform = platform,
      .arch = arch,
      .build_type = "release",
      .filename = "agent.zip",
      .storage_path = (test_root / "packages" / "agent.zip").string(),
      .size_bytes = 1,
      .sha256 =
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
      .manifest_json = zfleet::package::SerializeManifestJson(agent_manifest),
      .status = "validated",
      .uploaded_at = now,
      .validated_at = now,
  });
  database.PublishAgentPackage(agent_package_id, "agent", "stable", platform,
                               arch, "release", "test", now);
  database.UpsertAgentPackage(zfleet::server::AgentPackageRecord{
      .package_id = installer_package_id,
      .component = "installer",
      .version = "0.1.0",
      .platform = platform,
      .arch = arch,
      .build_type = "release",
      .filename = "installer.zip",
      .storage_path = (test_root / "packages" / "installer.zip").string(),
      .size_bytes = 1,
      .sha256 =
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
      .manifest_json =
          zfleet::package::SerializeManifestJson(installer_manifest),
      .status = "validated",
      .uploaded_at = now,
      .validated_at = now,
  });
  database.PublishAgentPackage(installer_package_id, "installer", "stable",
                               platform, arch, "release", "test", now);

  admin_server.Start();
  std::exception_ptr control_error;
  std::thread control_thread([&control_server, &control_error]() {
    try {
      control_server.Run();
    } catch (...) {
      control_error = std::current_exception();
    }
  });
  for (int attempt = 0;
       attempt < 50 && (control_server.port() == 0 || admin_server.port() == 0);
       ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  zfleet::agent::AgentConfig config{
      .control_url =
          "http://127.0.0.1:" + std::to_string(control_server.port()),
      .data_dir = test_root / "agent-data",
      .state_path = "agent-state.toml",
      .heartbeat_interval_seconds = 1,
      .reconnect_initial_delay_seconds = 1,
      .reconnect_max_delay_seconds = 2,
  };
  const auto agent_state =
      zfleet::agent::LoadOrCreateState(zfleet::agent::StatePathFor(config));
  zfleet::agent::AgentRuntime runtime(config);
  std::exception_ptr runtime_error;
  std::thread runtime_thread([&runtime, &runtime_error]() {
    try {
      runtime.Run();
    } catch (...) {
      runtime_error = std::current_exception();
    }
  });

  struct Cleanup {
    zfleet::agent::AgentRuntime& runtime;
    std::thread& runtime_thread;
    zfleet::server::AdminHttpServer& admin_server;
    zfleet::server::ControlServer& control_server;
    std::thread& control_thread;
    ~Cleanup() {
      runtime.RequestStop();
      if (runtime_thread.joinable()) {
        runtime_thread.join();
      }
      admin_server.Stop();
      control_server.Stop();
      if (control_thread.joinable()) {
        control_thread.join();
      }
    }
  } cleanup{runtime, runtime_thread, admin_server, control_server,
            control_thread};

  REQUIRE(WaitForSingleTextColumn(
      database_path, "select status from agents where agent_id = ?",
      agent_state.agent_id, "online"));

  database.EnqueueTask(zfleet::protocol::Task{
      .protocol_version = "v1",
      .task_id = "task-upgrade-flow-inventory",
      .agent_id = agent_state.agent_id,
      .task_type = zfleet::protocol::TaskType::collect_basic_inventory,
      .capability_level = zfleet::protocol::CapabilityLevel::readonly,
      .created_at = "2026-05-25T11:00:01Z",
      .expires_at = "2099-05-25T11:10:01Z",
      .input = zfleet::protocol::CollectBasicInventoryInput{},
  });
  REQUIRE(WaitForSingleTextColumn(database_path,
                                  "select state from tasks where task_id = ?",
                                  "task-upgrade-flow-inventory", "succeeded"));
  runtime.RequestStop();
  if (runtime_thread.joinable()) {
    runtime_thread.join();
  }

  const std::string options_request =
      "GET /api/v1/install/options?channel=stable&platform=" + platform +
      "&arch=" + arch + "&build_type=release HTTP/1.1\r\nHost: "
      "127.0.0.1\r\n\r\n";
  const auto options_response =
      SendHttpRequest(admin_server.port(), options_request);
  REQUIRE(options_response.status == 200);
  REQUIRE(options_response.body.find("\"agent\"") != std::string::npos);
  REQUIRE(options_response.body.find("\"installer\"") != std::string::npos);

  const auto stored_agent_platform = ReadSingleTextColumn(
      database_path, "select platform from agents where agent_id = ?",
      agent_state.agent_id);
  const auto stored_agent_version = ReadSingleTextColumn(
      database_path, "select agent_version from agents where agent_id = ?",
      agent_state.agent_id);
  const auto latest_asset_platform = ReadSingleTextColumn(
      database_path,
      "select os from asset_snapshots where agent_id = ? order by snapshot_id "
      "desc limit 1",
      agent_state.agent_id);
  const auto latest_asset_arch = ReadSingleTextColumn(
      database_path,
      "select arch from asset_snapshots where agent_id = ? order by snapshot_id "
      "desc limit 1",
      agent_state.agent_id);
  REQUIRE(stored_agent_platform == platform);
  REQUIRE(stored_agent_version == "0.1.0");
  REQUIRE(latest_asset_platform == platform);
  REQUIRE(latest_asset_arch == arch);

  const std::string token_body =
      "{\"purpose\":\"agent_register\",\"expires_at\":\"2099-01-01T00:00:"
      "00Z\"}";
  const auto token_request =
      "POST /api/v1/install/tokens HTTP/1.1\r\nHost: 127.0.0.1\r\n"
      "Content-Type: application/json\r\nContent-Length: " +
      std::to_string(token_body.size()) + "\r\n\r\n" + token_body;
  const auto token_response =
      SendHttpRequest(admin_server.port(), token_request);
  REQUIRE(token_response.status == 201);
  REQUIRE(token_response.body.find("\"token\"") != std::string::npos);

  const std::string upgrade_body =
      "{\"package_id\":\"" + agent_package_id +
      "\",\"set_by\":\"integration\",\"expires_at\":\"2099-05-25T12:00:00Z\"}";
  const auto upgrade_request =
      "POST /api/v1/agents/" + agent_state.agent_id +
      "/upgrade HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: "
      "application/json\r\nContent-Length: " +
      std::to_string(upgrade_body.size()) + "\r\n\r\n" + upgrade_body;
  const auto upgrade_response =
      SendHttpRequest(admin_server.port(), upgrade_request);
  REQUIRE(upgrade_response.status == 201);
  REQUIRE(upgrade_response.body.find("\"upgrade_state\":\"queued\"") !=
          std::string::npos);

  const auto upgrade_task_id = ReadSingleTextColumn(
      database_path,
      "select last_upgrade_task_id from agents where agent_id = ?",
      agent_state.agent_id);
  REQUIRE_FALSE(upgrade_task_id.empty());

  std::vector<std::uint8_t> update_result_body;
  const auto update_result_frame =
      EncodeEventFrame(TaskSucceededPackageUpdateEvent(
          "upgrade-result-1", agent_state.agent_id, upgrade_task_id,
          "2026-05-25T11:00:03Z", agent_package_id, "0.2.0"));
  update_result_body.insert(update_result_body.end(),
                            update_result_frame.begin(),
                            update_result_frame.end());
  PostHttp2Events(control_server.port(), std::move(update_result_body));

  std::vector<std::uint8_t> reconnect_body;
  const auto reconnect_frame = EncodeEventFrame(
      RegisterEvent("upgrade-reconnect-1", agent_state.agent_id,
                    "2026-05-25T11:00:04Z", "0.2.0", platform, arch));
  reconnect_body.insert(reconnect_body.end(), reconnect_frame.begin(),
                        reconnect_frame.end());
  PostHttp2Events(control_server.port(), std::move(reconnect_body));

  REQUIRE(WaitForSingleTextColumn(
      database_path, "select upgrade_state from agents where agent_id = ?",
      agent_state.agent_id, "succeeded"));
  REQUIRE(WaitForSingleTextColumn(
      database_path, "select agent_version from agents where agent_id = ?",
      agent_state.agent_id, "0.2.0"));

  if (runtime_error != nullptr) {
    std::rethrow_exception(runtime_error);
  }
  if (control_error != nullptr) {
    std::rethrow_exception(control_error);
  }
}
