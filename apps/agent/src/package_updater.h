#pragma once

#include "config.h"

#include "zfleet/protocol/message.h"

#include <string>

namespace zfleet::agent {

struct PackageUpdateExecutionResult {
  bool ok = false;
  bool stop_agent = false;
  zfleet::protocol::ErrorCode error_code =
      zfleet::protocol::ErrorCode::task_execution_failed;
  std::string message;
};

PackageUpdateExecutionResult ExecutePackageUpdate(
    const AgentConfig& config,
    const zfleet::protocol::PackageUpdateInput& input);

} // namespace zfleet::agent
