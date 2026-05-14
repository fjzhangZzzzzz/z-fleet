#pragma once

#include <filesystem>
#include <string>

namespace zfleet::agent {

struct AgentState {
  std::string agent_id;
};

AgentState LoadOrCreateState(const std::filesystem::path& state_path);

} // namespace zfleet::agent
