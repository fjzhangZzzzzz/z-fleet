#pragma once

#include <cstddef>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace zfleet::server {

struct ControlConnectionSnapshot {
  std::string connection_id;
  std::optional<std::string> agent_id;
  std::string connected_at;
  std::optional<std::string> disconnected_at;
  std::optional<std::string> last_heartbeat_at;
};

class Http2ConnectionRegistry {
 public:
  void OpenConnection(std::string connection_id, std::string connected_at);
  void CloseConnection(std::string_view connection_id,
                       std::string disconnected_at);
  void BindAgent(std::string_view connection_id,
                 std::string agent_id,
                 std::string observed_at);
  void RecordHeartbeat(std::string_view connection_id,
                       std::string agent_id,
                       std::string heartbeat_at);

  std::optional<ControlConnectionSnapshot> FindByConnection(
      std::string_view connection_id) const;
  std::optional<ControlConnectionSnapshot> FindByAgent(
      std::string_view agent_id) const;
  bool IsAgentOnline(std::string_view agent_id,
                     std::string_view stale_before) const;
  std::size_t ActiveConnectionCount() const;

 private:
  void BindAgentLocked(std::string_view connection_id,
                       std::string agent_id,
                       std::string observed_at);

  mutable std::mutex mutex_;
  std::map<std::string, ControlConnectionSnapshot, std::less<>>
      connections_by_id_;
  std::map<std::string, std::string, std::less<>> connection_by_agent_id_;
};

} // namespace zfleet::server
