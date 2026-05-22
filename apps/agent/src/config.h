#pragma once

#include "zfleet/core/log.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace zfleet::agent {

struct AgentConfig {
  std::string server_url = "http://127.0.0.1:8080";
  std::string control_url = "http://127.0.0.1:8081";
  std::filesystem::path data_dir = "data/agent";
  std::string state_file = "state.toml";
  std::uint32_t heartbeat_interval_seconds = 30;
  std::uint32_t reconnect_initial_delay_seconds = 1;
  std::uint32_t reconnect_max_delay_seconds = 30;
  zfleet::core::log::Config log{
      .level = zfleet::core::log::Level::kInfo,
      .enable_console = true,
      .file_path = "logs/zfleet-agent.log",
  };
};

AgentConfig LoadConfig(const std::optional<std::filesystem::path>& config_path);
std::filesystem::path StatePathFor(const AgentConfig& config);

} // namespace zfleet::agent
