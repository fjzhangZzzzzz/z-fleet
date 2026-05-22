#include "runtime.h"

#include "state.h"

#include "zfleet/core/log.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <utility>

namespace zfleet::agent {
namespace {

constexpr std::chrono::milliseconds kStopPollInterval{50};

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

  ZFLOG_ERROR(logger,
              "HTTP/2 control runtime is not implemented yet; use --once for "
              "diagnostics until P7 transport is wired");
  throw std::runtime_error("HTTP/2 control runtime is not implemented yet");
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
