#include "app.h"

#include "state.h"

#include "zfleet/core/log.h"
#include "zfleet/core/time.h"
#include "zfleet/core/version.h"
#include "zfleet/core/uuid.h"
#include "zfleet/platform/system.h"
#include "zfleet/protocol/json_codec.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>

namespace zfleet::agent {
namespace {

namespace asio = boost::asio;
namespace http = boost::beast::http;
using tcp = asio::ip::tcp;

struct ServerEndpoint {
  std::string host;
  std::string port;
  std::string base_target;
};

ServerEndpoint ParseServerUrl(std::string_view server_url) {
  constexpr std::string_view kHttpScheme = "http://";
  if (!server_url.starts_with(kHttpScheme)) {
    throw std::runtime_error("only http:// server_url is supported in v0.1");
  }

  const auto authority_begin = kHttpScheme.size();
  const auto path_begin = server_url.find('/', authority_begin);
  const auto authority =
      server_url.substr(authority_begin, path_begin - authority_begin);
  if (authority.empty()) {
    throw std::runtime_error("server_url host must not be empty");
  }

  const auto colon = authority.rfind(':');
  ServerEndpoint endpoint;
  if (colon == std::string_view::npos) {
    endpoint.host = std::string(authority);
    endpoint.port = "80";
  } else {
    endpoint.host = std::string(authority.substr(0, colon));
    endpoint.port = std::string(authority.substr(colon + 1));
  }

  if (endpoint.host.empty() || endpoint.port.empty()) {
    throw std::runtime_error("server_url must include a valid host and port");
  }

  endpoint.base_target =
      path_begin == std::string_view::npos ? "" : std::string(server_url.substr(path_begin));
  if (!endpoint.base_target.empty() && endpoint.base_target.back() == '/') {
    endpoint.base_target.pop_back();
  }
  return endpoint;
}

std::string JoinTarget(const std::string& base_target, std::string_view suffix) {
  if (base_target.empty()) {
    return std::string(suffix);
  }
  return base_target + std::string(suffix);
}

template <typename Request>
zfleet::protocol::StatusResponse PostJson(const ServerEndpoint& endpoint,
                                          std::string_view target,
                                          const Request& payload) {
  asio::io_context io_context;
  tcp::resolver resolver(io_context);
  tcp::socket socket(io_context);
  const auto endpoints = resolver.resolve(endpoint.host, endpoint.port);
  asio::connect(socket, endpoints);

  http::request<http::string_body> request{http::verb::post,
                                           std::string(target), 11};
  request.set(http::field::host, endpoint.host);
  request.set(http::field::content_type, "application/json");
  request.body() = nlohmann::json(payload).dump();
  request.prepare_payload();
  http::write(socket, request);

  boost::beast::flat_buffer buffer;
  http::response<http::string_body> response;
  http::read(socket, buffer, response);

  const auto response_json = nlohmann::json::parse(response.body());
  if (response.result() == http::status::ok) {
    return response_json.get<zfleet::protocol::StatusResponse>();
  }

  if (response_json.contains("error_code")) {
    const auto error = response_json.get<zfleet::protocol::ErrorResponse>();
    throw std::runtime_error(
        "server returned " + std::to_string(response.result_int()) + " " +
        std::string(zfleet::protocol::ToString(error.error_code)) + ": " +
        error.message);
  }

  throw std::runtime_error("server returned unexpected response");
}

template <typename Response>
Response GetJson(const ServerEndpoint& endpoint,
                 std::string_view target,
                 std::string_view request_id) {
  asio::io_context io_context;
  tcp::resolver resolver(io_context);
  tcp::socket socket(io_context);
  const auto endpoints = resolver.resolve(endpoint.host, endpoint.port);
  asio::connect(socket, endpoints);

  http::request<http::string_body> request{http::verb::get,
                                           std::string(target), 11};
  request.set(http::field::host, endpoint.host);
  request.set("X-Request-Id", request_id);
  http::write(socket, request);

  boost::beast::flat_buffer buffer;
  http::response<http::string_body> response;
  http::read(socket, buffer, response);

  const auto response_json = nlohmann::json::parse(response.body());
  if (response.result() == http::status::ok) {
    return response_json.get<Response>();
  }

  if (response_json.contains("error_code")) {
    const auto error = response_json.get<zfleet::protocol::ErrorResponse>();
    throw std::runtime_error(
        "server returned " + std::to_string(response.result_int()) + " " +
        std::string(zfleet::protocol::ToString(error.error_code)) + ": " +
        error.message);
  }

  throw std::runtime_error("server returned unexpected response");
}

zfleet::protocol::RegistrationRequest MakeRegistrationRequest(
    const AgentState& state) {
  return zfleet::protocol::RegistrationRequest{
      .protocol_version = std::string(zfleet::protocol::protocol_version()),
      .request_id = zfleet::core::GenerateUuid(),
      .agent_id = state.agent_id,
      .occurred_at = zfleet::core::NowUtcRfc3339(),
      .agent_version = std::string(zfleet::core::version()),
      .hostname = zfleet::platform::hostname(),
      .os = std::string(zfleet::platform::os_name()),
      .arch = zfleet::platform::architecture_name(),
  };
}

zfleet::protocol::HeartbeatRequest MakeHeartbeatRequest(
    const AgentState& state) {
  return zfleet::protocol::HeartbeatRequest{
      .protocol_version = std::string(zfleet::protocol::protocol_version()),
      .request_id = zfleet::core::GenerateUuid(),
      .agent_id = state.agent_id,
      .occurred_at = zfleet::core::NowUtcRfc3339(),
      .agent_version = std::string(zfleet::core::version()),
  };
}

zfleet::protocol::AssetSnapshotRequest MakeAssetSnapshotRequest(
    const AgentState& state) {
  return zfleet::protocol::AssetSnapshotRequest{
      .protocol_version = std::string(zfleet::protocol::protocol_version()),
      .request_id = zfleet::core::GenerateUuid(),
      .agent_id = state.agent_id,
      .occurred_at = zfleet::core::NowUtcRfc3339(),
      .hostname = zfleet::platform::hostname(),
      .os = std::string(zfleet::platform::os_name()),
      .os_version = std::nullopt,
      .arch = zfleet::platform::architecture_name(),
      .agent_version = std::string(zfleet::core::version()),
  };
}

zfleet::protocol::CollectBasicInventoryResult ExecuteCollectBasicInventory() {
  return zfleet::protocol::CollectBasicInventoryResult{
      .hostname = zfleet::platform::hostname(),
      .os = std::string(zfleet::platform::os_name()),
      .arch = zfleet::platform::architecture_name(),
      .agent_version = std::string(zfleet::core::version()),
  };
}

zfleet::protocol::TaskResultRequest MakeTaskResultRequest(
    const AgentState& state,
    const zfleet::protocol::Task& task) {
  zfleet::protocol::TaskResultRequest request{
      .protocol_version = std::string(zfleet::protocol::protocol_version()),
      .request_id = zfleet::core::GenerateUuid(),
      .task_id = task.task_id,
      .agent_id = state.agent_id,
      .task_type = task.task_type,
      .occurred_at = zfleet::core::NowUtcRfc3339(),
      .status = zfleet::protocol::TaskExecutionStatus::failed,
      .result = std::nullopt,
      .error = std::nullopt,
  };

  switch (task.task_type) {
    case zfleet::protocol::TaskType::collect_basic_inventory:
      request.status = zfleet::protocol::TaskExecutionStatus::succeeded;
      request.result = ExecuteCollectBasicInventory();
      break;
  }

  return request;
}

zfleet::protocol::TaskRunningRequest MakeTaskRunningRequest(
    const AgentState& state,
    const zfleet::protocol::Task& task) {
  return zfleet::protocol::TaskRunningRequest{
      .protocol_version = std::string(zfleet::protocol::protocol_version()),
      .request_id = zfleet::core::GenerateUuid(),
      .task_id = task.task_id,
      .agent_id = state.agent_id,
      .task_type = task.task_type,
      .occurred_at = zfleet::core::NowUtcRfc3339(),
  };
}

} // namespace

RunResult RunOnce(const AgentConfig& config) {
  const auto state_path = StatePathFor(config);
  const auto state = LoadOrCreateState(state_path);
  const auto endpoint = ParseServerUrl(config.server_url);
  const auto logger = zfleet::core::log::Component("agent").With(
      {{"agent_id", state.agent_id},
       {"server_url", config.server_url},
       {"data_dir", config.data_dir.string()}});

  ZFLOG_INFO(logger,
             "{} agent {} protocol {} on {} hostname={} arch={}",
             zfleet::core::project_name(),
             zfleet::core::version(),
             zfleet::protocol::protocol_version(),
             zfleet::platform::os_name(),
             zfleet::platform::hostname(),
             zfleet::platform::architecture_name());

  const auto registration = MakeRegistrationRequest(state);
  const auto register_response =
      PostJson(endpoint,
               JoinTarget(endpoint.base_target, "/v1/agents/register"),
               registration);
  ZFLOG_INFO(logger,
             "registration accepted request_id={} server_time={}",
             register_response.request_id,
             register_response.server_time);

  const auto heartbeat = MakeHeartbeatRequest(state);
  const auto heartbeat_response =
      PostJson(endpoint,
               JoinTarget(endpoint.base_target,
                          "/v1/agents/" + state.agent_id + "/heartbeat"),
               heartbeat);
  ZFLOG_INFO(logger,
             "heartbeat stored request_id={} server_time={}",
             heartbeat_response.request_id,
             heartbeat_response.server_time);

  const auto assets = MakeAssetSnapshotRequest(state);
  const auto assets_response =
      PostJson(endpoint,
               JoinTarget(endpoint.base_target,
                          "/v1/agents/" + state.agent_id + "/assets"),
               assets);
  ZFLOG_INFO(logger,
             "asset snapshot stored request_id={} server_time={}",
             assets_response.request_id,
             assets_response.server_time);

  const auto poll_request_id = zfleet::core::GenerateUuid();
  const auto poll_response = GetJson<zfleet::protocol::TaskPollResponse>(
      endpoint,
      JoinTarget(endpoint.base_target,
                 "/v1/agents/" + state.agent_id + "/tasks/poll"),
      poll_request_id);
  if (poll_response.status == zfleet::protocol::TaskPollStatus::idle ||
      !poll_response.task.has_value()) {
    ZFLOG_INFO(logger,
               "task poll idle request_id={} server_time={}",
               poll_response.request_id,
               poll_response.server_time);
    return RunResult{.agent_id = state.agent_id, .completed_task_id = std::nullopt};
  }

  const auto& task = *poll_response.task;
  ZFLOG_INFO(logger,
             "task assigned request_id={} task_id={} task_type={}",
             poll_response.request_id,
             task.task_id,
             zfleet::protocol::ToString(task.task_type));

  const auto task_running = MakeTaskRunningRequest(state, task);
  const auto task_running_response =
      PostJson(endpoint,
               JoinTarget(endpoint.base_target,
                          "/v1/tasks/" + task.task_id + "/running"),
               task_running);
  ZFLOG_INFO(logger,
             "task running accepted request_id={} task_id={} server_time={}",
             task_running_response.request_id,
             task.task_id,
             task_running_response.server_time);

  const auto task_result = MakeTaskResultRequest(state, task);
  const auto task_result_response =
      PostJson(endpoint,
               JoinTarget(endpoint.base_target,
                          "/v1/tasks/" + task.task_id + "/result"),
               task_result);
  ZFLOG_INFO(logger,
             "task result accepted request_id={} task_id={} server_time={}",
             task_result_response.request_id,
             task.task_id,
             task_result_response.server_time);

  return RunResult{.agent_id = state.agent_id, .completed_task_id = task.task_id};
}

} // namespace zfleet::agent
