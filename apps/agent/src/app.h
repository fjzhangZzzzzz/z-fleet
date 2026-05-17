#pragma once

#include "config.h"

#include <optional>
#include <string>

namespace zfleet::agent {

struct RunResult {
  std::string agent_id;
  std::optional<std::string> completed_task_id;
};

RunResult RunOnce(const AgentConfig& config);

} // namespace zfleet::agent
