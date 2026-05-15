#pragma once

#include "config.h"

#include <string>

namespace zfleet::agent {

struct RunResult {
  std::string agent_id;
};

RunResult RunOnce(const AgentConfig& config);

} // namespace zfleet::agent
