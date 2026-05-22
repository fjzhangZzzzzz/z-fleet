#pragma once

#include "config.h"

#include <atomic>
#include <cstdint>
#include <string>

namespace zfleet::agent {

struct RuntimeResult {
  std::string agent_id;
  std::uint64_t heartbeat_count = 0;
};

class AgentRuntime {
 public:
  explicit AgentRuntime(AgentConfig config);

  RuntimeResult Run();
  void RequestStop() noexcept;
  bool stop_requested() const noexcept;

 private:
  bool SleepUntilStop(std::uint32_t seconds) const;

  AgentConfig config_;
  std::atomic_bool stop_requested_{false};
};

} // namespace zfleet::agent
