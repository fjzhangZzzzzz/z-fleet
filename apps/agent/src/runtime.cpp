#include "runtime.h"

#include "command_decoder.h"
#include "http2_control_client.h"
#include "state.h"

#include "zfleet/core/log.h"
#include "zfleet/core/time.h"
#include "zfleet/core/uuid.h"
#include "zfleet/core/version.h"
#include "zfleet/platform/system.h"
#include "zfleet/protocol/message.h"
#include "zfleet/protocol/v1/agent_control.pb.h"
#include "zfleet/transport/frame_codec.h"
#include "zfleet/transport/http2_session.h"

#include <nghttp2/nghttp2.h>

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

namespace proto = zfleet::protocol::v1;

std::vector<std::uint8_t> EncodeEventFrame(const proto::AgentEvent& event) {
  std::string bytes;
  if (!event.SerializeToString(&bytes)) {
    throw std::runtime_error("failed to serialize agent event");
  }
  return zfleet::transport::EncodeFrame(std::span<const std::uint8_t>{
      reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()});
}

proto::AgentEvent BuildRegisterEvent(const std::string& agent_id,
                                     const std::string& registration_token) {
  proto::AgentEvent event;
  event.set_protocol_version(std::string(zfleet::protocol::protocol_version()));
  event.set_message_id(zfleet::core::GenerateUuid());
  event.set_agent_id(agent_id);
  event.set_occurred_at(zfleet::core::NowUtcRfc3339());
  auto* register_event = event.mutable_register_();
  register_event->set_agent_version(std::string(zfleet::core::version()));
  register_event->set_hostname(zfleet::platform::hostname());
  register_event->set_os(std::string(zfleet::platform::os_name()));
  register_event->set_arch(zfleet::platform::architecture_name());
  register_event->set_registration_token(registration_token);
  return event;
}

proto::AgentEvent BuildHeartbeatEvent(const std::string& agent_id) {
  proto::AgentEvent event;
  event.set_protocol_version(std::string(zfleet::protocol::protocol_version()));
  event.set_message_id(zfleet::core::GenerateUuid());
  event.set_agent_id(agent_id);
  event.set_occurred_at(zfleet::core::NowUtcRfc3339());
  event.mutable_heartbeat()->set_agent_version(
      std::string(zfleet::core::version()));
  return event;
}

proto::AgentEvent BuildTaskRunningEvent(const std::string& agent_id,
                                        const proto::TaskAssignedCommand& task) {
  proto::AgentEvent event;
  event.set_protocol_version(std::string(zfleet::protocol::protocol_version()));
  event.set_message_id(zfleet::core::GenerateUuid());
  event.set_agent_id(agent_id);
  event.set_occurred_at(zfleet::core::NowUtcRfc3339());
  auto* running = event.mutable_task_running();
  running->set_task_id(task.task_id());
  running->set_task_type(task.task_type());
  return event;
}

proto::AgentEvent BuildTaskSucceededEvent(
    const std::string& agent_id,
    const proto::TaskAssignedCommand& task) {
  proto::AgentEvent event;
  event.set_protocol_version(std::string(zfleet::protocol::protocol_version()));
  event.set_message_id(zfleet::core::GenerateUuid());
  event.set_agent_id(agent_id);
  event.set_occurred_at(zfleet::core::NowUtcRfc3339());
  auto* result = event.mutable_task_result();
  result->set_task_id(task.task_id());
  result->set_task_type(task.task_type());
  result->set_status(proto::TASK_EXECUTION_STATUS_SUCCEEDED);
  auto* inventory = result->mutable_collect_basic_inventory();
  inventory->set_hostname(zfleet::platform::hostname());
  inventory->set_os(std::string(zfleet::platform::os_name()));
  inventory->set_arch(zfleet::platform::architecture_name());
  inventory->set_agent_version(std::string(zfleet::core::version()));
  return event;
}

std::vector<std::uint8_t> BuildEnvelopeBody(
    std::initializer_list<proto::AgentEvent> events) {
  std::vector<std::uint8_t> body;
  for (const auto& event : events) {
    const auto frame = EncodeEventFrame(event);
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
    const auto bootstrap_body = BuildEnvelopeBody(
        {BuildRegisterEvent(state.agent_id, config_.registration_token),
         BuildHeartbeatEvent(state.agent_id)});
    const auto response = client.PostEvents(bootstrap_body);
    if (response.status != "200") {
      throw std::runtime_error("agent bootstrap events were rejected");
    }
    ++result.heartbeat_count;
  };

  auto send_heartbeat = [&]() {
    const auto heartbeat_body =
        BuildEnvelopeBody({BuildHeartbeatEvent(state.agent_id)});
    const auto response = client.PostEvents(heartbeat_body);
    if (response.status != "200") {
      throw std::runtime_error("heartbeat was rejected");
    }
    ++result.heartbeat_count;
  };

  auto handle_command = [&](const proto::ServerCommand& command) {
    if (command.payload_case() == proto::ServerCommand::kError) {
      throw std::runtime_error("server control error " +
                               proto::ErrorCode_Name(command.error().code()) +
                               ": " + command.error().message());
    }
    if (command.payload_case() != proto::ServerCommand::kTaskAssigned) {
      ZFLOG_WARN(logger, "unsupported server command payload");
      return;
    }

    const auto& task = command.task_assigned();
    if (task.task_type() != proto::TASK_TYPE_COLLECT_BASIC_INVENTORY) {
      ZFLOG_WARN(logger, "unsupported task type received from server");
      return;
    }

    ZFLOG_INFO(logger, "task assigned task_id={}", task.task_id());
    const auto running_body = BuildEnvelopeBody(
        {BuildTaskRunningEvent(state.agent_id, task)});
    const auto running_response = client.PostEvents(running_body);
    if (running_response.status != "200") {
      throw std::runtime_error("task running event was rejected with status " +
                               running_response.status);
    }

    const auto result_body = BuildEnvelopeBody(
        {BuildTaskSucceededEvent(state.agent_id, task)});
    const auto task_response = client.PostEvents(result_body);
    if (task_response.status != "200") {
      throw std::runtime_error("task result event was rejected with status " +
                               task_response.status);
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
