#pragma once

#include "zfleet/core/log.h"

#include <filesystem>
#include <optional>
#include <string>

namespace zfleet::agent {

struct AgentConfig {
  std::string server_url = "http://127.0.0.1:8080";
  std::filesystem::path data_dir = "data/agent";
  std::string state_file = "state.toml";
  zfleet::core::log::Config log{
      .level = zfleet::core::log::Level::kInfo,
      .enable_console = true,
      .file_path = "logs/zfleet-agent.log",
  };
};

AgentConfig LoadConfig(const std::optional<std::filesystem::path>& config_path);
std::filesystem::path StatePathFor(const AgentConfig& config);

} // namespace zfleet::agent
