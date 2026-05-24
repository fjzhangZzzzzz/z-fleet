#pragma once

#include "zfleet/core/log.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace zfleet::agent {

struct AgentConfig {
  std::optional<std::filesystem::path> install_dir = std::nullopt;
  std::string control_url = "http://127.0.0.1:8081";
  std::string registration_token;
  std::filesystem::path data_dir = "data/agent";
  std::filesystem::path state_path = "state.toml";
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
std::filesystem::path DefaultConfigPath(
    const std::optional<std::filesystem::path>& install_dir);
void SaveConfig(const AgentConfig& config,
                const std::filesystem::path& config_path);
void ResolveConfigPaths(AgentConfig* config);
std::filesystem::path StatePathFor(const AgentConfig& config);

} // namespace zfleet::agent
