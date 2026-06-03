#include "runtime.h"

#include "command_decoder.h"
#include "http2_control_client.h"
#include "package_updater.h"
#include "state.h"

#include "zfleet/core/log.h"
#include "zfleet/core/time.h"
#include "zfleet/core/uuid.h"
#include "zfleet/core/version.h"
#include "zfleet/platform/system.h"
#include "zfleet/protocol/control_codec.h"
#include "zfleet/transport/frame_codec.h"
#include "zfleet/transport/http2_session.h"

#include "zfleet/transport/nghttp2_compat.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <stdexcept>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace zfleet::agent {
namespace {

constexpr std::chrono::milliseconds kStopPollInterval{50};
constexpr std::chrono::milliseconds kCommandPumpInterval{50};

zfleet::protocol::AgentEvent BuildRegisterEvent(
    const std::string& agent_id, const std::string& registration_token) {
  const auto protocol_version =
      std::string(zfleet::protocol::protocol_version());
  const auto request_id = zfleet::core::GenerateUuid();
  const auto occurred_at = zfleet::core::NowUtcRfc3339();
  const auto hostname = zfleet::platform::hostname();
  const auto os = std::string(zfleet::platform::os_name());
  const auto arch = zfleet::platform::architecture_name();
  return zfleet::protocol::AgentEvent{zfleet::protocol::AgentRegistration{
      .protocol_version = protocol_version,
      .request_id = request_id,
      .agent_id = agent_id,
      .occurred_at = occurred_at,
      .agent_version = std::string(zfleet::core::version()),
      .hostname = hostname,
      .os = os,
      .arch = arch,
      .registration_token = registration_token.empty()
                                 ? std::optional<std::string>{}
                                 : std::optional<std::string>{
                                       registration_token},
  }};
}

zfleet::protocol::AgentEvent BuildHeartbeatEvent(const std::string& agent_id) {
  return zfleet::protocol::AgentEvent{zfleet::protocol::AgentHeartbeat{
      .protocol_version = std::string(zfleet::protocol::protocol_version()),
      .request_id = zfleet::core::GenerateUuid(),
      .agent_id = agent_id,
      .occurred_at = zfleet::core::NowUtcRfc3339(),
      .agent_version = std::string(zfleet::core::version()),
  }};
}

zfleet::protocol::AgentEvent BuildTaskRunningEvent(
    const std::string& agent_id, const zfleet::protocol::Task& task) {
  return zfleet::protocol::AgentEvent{zfleet::protocol::TaskRunning{
      .protocol_version = std::string(zfleet::protocol::protocol_version()),
      .request_id = zfleet::core::GenerateUuid(),
      .task_id = task.task_id,
      .agent_id = agent_id,
      .task_type = task.task_type,
      .occurred_at = zfleet::core::NowUtcRfc3339(),
  }};
}

zfleet::protocol::AgentEvent BuildTaskSucceededEvent(
    const std::string& agent_id, const zfleet::protocol::Task& task) {
  return zfleet::protocol::AgentEvent{zfleet::protocol::TaskResult{
      .protocol_version = std::string(zfleet::protocol::protocol_version()),
      .request_id = zfleet::core::GenerateUuid(),
      .task_id = task.task_id,
      .agent_id = agent_id,
      .task_type = task.task_type,
      .occurred_at = zfleet::core::NowUtcRfc3339(),
      .status = zfleet::protocol::TaskExecutionStatus::succeeded,
      .result = zfleet::protocol::CollectBasicInventoryResult{
          .hostname = zfleet::platform::hostname(),
          .os = std::string(zfleet::platform::os_name()),
          .arch = zfleet::platform::architecture_name(),
          .agent_version = std::string(zfleet::core::version()),
      },
      .error = std::nullopt,
  }};
}

zfleet::protocol::AgentEvent BuildPackageUpdateResultEvent(
    const std::string& agent_id,
    const zfleet::protocol::Task& task,
    const PackageUpdateExecutionResult& execution) {
  const auto& input = std::get<zfleet::protocol::PackageUpdateInput>(task.input);
  zfleet::protocol::TaskResult result{
      .protocol_version = std::string(zfleet::protocol::protocol_version()),
      .request_id = zfleet::core::GenerateUuid(),
      .task_id = task.task_id,
      .agent_id = agent_id,
      .task_type = task.task_type,
      .occurred_at = zfleet::core::NowUtcRfc3339(),
      .status = execution.ok ? zfleet::protocol::TaskExecutionStatus::succeeded
                             : zfleet::protocol::TaskExecutionStatus::failed,
      .result = zfleet::protocol::PackageUpdateResult{
          .component = input.component,
          .package_id = input.package_id,
          .version = input.version,
          .state = execution.ok ? "applied" : "failed",
          .error_code = execution.ok
                            ? std::string{}
                            : std::string(
                                  zfleet::protocol::ToString(
                                      execution.error_code)),
          .error_message = execution.ok ? std::string{} : execution.message,
      },
      .error = std::nullopt,
  };
  if (!execution.ok) {
    result.error = zfleet::protocol::TaskError{
        .error_code = execution.error_code,
        .message = execution.message,
        .retryable = false,
    };
  }
  return zfleet::protocol::AgentEvent{std::move(result)};
}

std::vector<std::uint8_t> BuildEnvelopeBody(
    std::initializer_list<zfleet::protocol::AgentEvent> events) {
  std::vector<std::uint8_t> body;
  for (const auto& event : events) {
    const auto payload = zfleet::protocol::EncodeAgentEventPayload(event);
    const auto frame = zfleet::transport::EncodeFrame(
        std::span<const std::uint8_t>{payload.data(), payload.size()});
    body.insert(body.end(), frame.begin(), frame.end());
  }
  return body;
}

} // namespace

AgentRuntime::AgentRuntime(AgentConfig config)
    : config_(std::move(config)) {}

RuntimeResult AgentRuntime::Run() {
  const auto state_path = StatePathFor(config_);
  const auto state = LoadOrCreateState(state_path);
  auto logger = zfleet::core::log::Component("agent").With(
      {{"agent_id", state.agent_id},
       {"control_url", config_.control_url},
       {"data_dir", config_.data_dir.string()}});

  ZFLOG_INFO(logger, "starting HTTP/2 control runtime");

  RuntimeResult result{.agent_id = state.agent_id};
  Http2ControlClient client(config_.control_url);
  const auto heartbeat_interval =
      std::max<std::uint32_t>(1, config_.heartbeat_interval_seconds);
  const auto initial_backoff =
      std::max<std::uint32_t>(1, config_.reconnect_initial_delay_seconds);
  const auto max_backoff =
      std::max<std::uint32_t>(initial_backoff, config_.reconnect_max_delay_seconds);

  auto send_connection_bootstrap = [&]() {
    const auto register_event =
        BuildRegisterEvent(state.agent_id, config_.registration_token);
    const auto heartbeat_event = BuildHeartbeatEvent(state.agent_id);
    const auto bootstrap_body =
        BuildEnvelopeBody({register_event, heartbeat_event});
    const auto response =
        client.PostEvents(std::span<const std::uint8_t>{bootstrap_body});
    if (response.status != "200") {
      throw std::runtime_error("agent bootstrap events were rejected");
    }
    ++result.heartbeat_count;
  };

  auto send_heartbeat = [&]() {
    const auto heartbeat_body =
        BuildEnvelopeBody({BuildHeartbeatEvent(state.agent_id)});
    const auto response =
        client.PostEvents(std::span<const std::uint8_t>{heartbeat_body});
    if (response.status != "200") {
      throw std::runtime_error("heartbeat was rejected");
    }
    ++result.heartbeat_count;
  };

  auto handle_command = [&](const zfleet::protocol::ServerCommand& command) {
    if (const auto* error =
            std::get_if<zfleet::protocol::ServerError>(&command.payload)) {
      throw std::runtime_error(
          "server control error " +
          std::string(zfleet::protocol::ToString(error->error_code)) + ": " +
          error->message);
    }
    const auto* task = std::get_if<zfleet::protocol::Task>(&command.payload);
    if (task == nullptr) {
      ZFLOG_WARN(logger, "unsupported server command payload");
      return;
    }

    ZFLOG_INFO(logger, "task assigned task_id={}", task->task_id);
    const auto running_body = BuildEnvelopeBody(
        {BuildTaskRunningEvent(state.agent_id, *task)});
    const auto running_response =
        client.PostEvents(std::span<const std::uint8_t>{running_body});
    if (running_response.status != "200") {
      throw std::runtime_error("task running event was rejected with status " +
                               running_response.status);
    }

    zfleet::protocol::AgentEvent task_result;
    bool stop_after_result = false;
    if (task->task_type == zfleet::protocol::TaskType::collect_basic_inventory) {
      task_result = BuildTaskSucceededEvent(state.agent_id, *task);
    } else {
      const auto execution =
          ExecutePackageUpdate(
              config_,
              std::get<zfleet::protocol::PackageUpdateInput>(task->input));
      task_result =
          BuildPackageUpdateResultEvent(state.agent_id, *task, execution);
      stop_after_result = execution.ok && execution.stop_agent;
    }
    const auto result_body = BuildEnvelopeBody({task_result});
    const auto task_response =
        client.PostEvents(std::span<const std::uint8_t>{result_body});
    if (task_response.status != "200") {
      throw std::runtime_error("task result event was rejected with status " +
                               task_response.status);
    }
    if (stop_after_result) {
      RequestStop();
    }
  };

  std::uint32_t backoff = initial_backoff;
  while (!stop_requested()) {
    try {
      ZFLOG_INFO(logger, "connecting HTTP/2 control channel");
      client.Connect();
      ZFLOG_INFO(logger, "HTTP/2 control channel connected");
      ZFLOG_INFO(logger, "sending register and initial heartbeat");
      send_connection_bootstrap();
      ZFLOG_INFO(logger, "bootstrap event batch accepted");
      const auto command_status =
          client.StartCommandStream(zfleet::core::GenerateUuid());
      if (command_status != "200") {
        throw std::runtime_error("command stream was rejected");
      }
      ZFLOG_INFO(logger, "command stream opened");

      zfleet::transport::FrameDecoder command_decoder;
      auto next_heartbeat = std::chrono::steady_clock::now() +
                            std::chrono::seconds(heartbeat_interval);
      while (!stop_requested()) {
        if (!client.command_stream_open()) {
          const auto error_code = client.command_stream_error_code();
          if (error_code.has_value() && *error_code != NGHTTP2_NO_ERROR) {
            throw std::runtime_error("command stream reset: " +
                                     std::string(zfleet::transport::Http2ErrorMessage(
                                         *error_code)));
          }
          throw std::runtime_error("command stream closed");
        }

        client.PumpFor(kCommandPumpInterval);
        const auto command_bytes = client.DrainCommandBytes();
        if (!command_bytes.empty()) {
          const auto commands = DecodeServerCommands(
              &command_decoder,
              std::span<const std::uint8_t>{command_bytes.data(),
                                            command_bytes.size()});
          for (const auto& command : commands) {
            handle_command(command);
          }
        }

        if (stop_requested()) {
          break;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= next_heartbeat) {
          send_heartbeat();
          next_heartbeat = now + std::chrono::seconds(heartbeat_interval);
        }
      }

      client.Close();
      return result;
    } catch (const std::exception& ex) {
      ZFLOG_WARN(logger, "agent runtime connection failed: {}", ex.what());
      client.Close();
      if (!SleepUntilStop(backoff)) {
        break;
      }
      backoff = std::min(backoff * 2, max_backoff);
    }
  }

  client.Close();
  return result;
}

void AgentRuntime::RequestStop() noexcept {
  stop_requested_.store(true, std::memory_order_relaxed);
}

bool AgentRuntime::stop_requested() const noexcept {
  return stop_requested_.load(std::memory_order_relaxed);
}

bool AgentRuntime::SleepUntilStop(std::uint32_t seconds) const {
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(seconds);
  while (std::chrono::steady_clock::now() < deadline && !stop_requested()) {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    const auto sleep_for = std::min(remaining, kStopPollInterval);
    std::this_thread::sleep_for(sleep_for);
  }
  return !stop_requested();
}

} // namespace zfleet::agent
