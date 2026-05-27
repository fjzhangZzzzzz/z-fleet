#include "control_connection_registry.h"

#include <stdexcept>
#include <utility>

namespace zfleet::server {

void ControlConnectionRegistry::OpenConnection(std::string connection_id,
                                               std::string connected_at) {
  if (connection_id.empty()) {
    throw std::invalid_argument("connection_id must not be empty");
  }
  if (connected_at.empty()) {
    throw std::invalid_argument("connected_at must not be empty");
  }

  std::lock_guard lock(mutex_);
  const auto id = connection_id;
  connections_by_id_.insert_or_assign(
      id, ControlConnectionSnapshot{
              .connection_id = std::move(connection_id),
              .agent_id = std::nullopt,
              .connected_at = std::move(connected_at),
              .disconnected_at = std::nullopt,
              .last_heartbeat_at = std::nullopt,
          });
}

std::optional<ClosedConnectionSnapshot>
ControlConnectionRegistry::CloseConnection(std::string_view connection_id,
                                           std::string disconnected_at) {
  std::lock_guard lock(mutex_);
  const auto connection = connections_by_id_.find(connection_id);
  if (connection == connections_by_id_.end()) {
    return std::nullopt;
  }

  ClosedConnectionSnapshot closed{
      .connection_id = connection->second.connection_id,
      .agent_id = connection->second.agent_id,
      .disconnected_at = disconnected_at,
      .was_current_agent_connection = false,
  };
  connection->second.disconnected_at = std::move(disconnected_at);
  if (connection->second.agent_id.has_value()) {
    const auto agent =
        connection_by_agent_id_.find(*connection->second.agent_id);
    if (agent != connection_by_agent_id_.end() &&
        agent->second == connection->second.connection_id) {
      connection_by_agent_id_.erase(agent);
      closed.was_current_agent_connection = true;
    }
  }
  connections_by_id_.erase(connection);
  return closed;
}

void ControlConnectionRegistry::BindAgent(std::string_view connection_id,
                                          std::string agent_id,
                                          std::string observed_at) {
  if (agent_id.empty()) {
    throw std::invalid_argument("agent_id must not be empty");
  }
  if (observed_at.empty()) {
    throw std::invalid_argument("observed_at must not be empty");
  }

  std::lock_guard lock(mutex_);
  BindAgentLocked(connection_id, std::move(agent_id), std::move(observed_at));
}

void ControlConnectionRegistry::RecordHeartbeat(std::string_view connection_id,
                                                std::string agent_id,
                                                std::string heartbeat_at) {
  if (agent_id.empty()) {
    throw std::invalid_argument("agent_id must not be empty");
  }
  if (heartbeat_at.empty()) {
    throw std::invalid_argument("heartbeat_at must not be empty");
  }

  std::lock_guard lock(mutex_);
  BindAgentLocked(connection_id, std::move(agent_id), heartbeat_at);
  auto connection = connections_by_id_.find(connection_id);
  if (connection != connections_by_id_.end()) {
    connection->second.last_heartbeat_at = std::move(heartbeat_at);
  }
}

std::optional<ControlConnectionSnapshot>
ControlConnectionRegistry::FindByConnection(
    std::string_view connection_id) const {
  std::lock_guard lock(mutex_);
  const auto connection = connections_by_id_.find(connection_id);
  if (connection == connections_by_id_.end()) {
    return std::nullopt;
  }
  return connection->second;
}

std::optional<ControlConnectionSnapshot> ControlConnectionRegistry::FindByAgent(
    std::string_view agent_id) const {
  std::lock_guard lock(mutex_);
  const auto agent = connection_by_agent_id_.find(agent_id);
  if (agent == connection_by_agent_id_.end()) {
    return std::nullopt;
  }

  const auto connection = connections_by_id_.find(agent->second);
  if (connection == connections_by_id_.end() ||
      connection->second.disconnected_at.has_value()) {
    return std::nullopt;
  }
  return connection->second;
}

bool ControlConnectionRegistry::IsAgentOnline(
    std::string_view agent_id, std::string_view stale_before) const {
  const auto connection = FindByAgent(agent_id);
  if (!connection.has_value() || !connection->last_heartbeat_at.has_value()) {
    return false;
  }
  return *connection->last_heartbeat_at >= stale_before;
}

std::size_t ControlConnectionRegistry::ActiveConnectionCount() const {
  std::lock_guard lock(mutex_);
  std::size_t count = 0;
  for (const auto& [_, connection] : connections_by_id_) {
    if (!connection.disconnected_at.has_value()) {
      ++count;
    }
  }
  return count;
}

void ControlConnectionRegistry::BindAgentLocked(std::string_view connection_id,
                                                std::string agent_id,
                                                std::string observed_at) {
  auto connection = connections_by_id_.find(connection_id);
  if (connection == connections_by_id_.end()) {
    connections_by_id_.insert_or_assign(
        std::string(connection_id),
        ControlConnectionSnapshot{
            .connection_id = std::string(connection_id),
            .agent_id = std::nullopt,
            .connected_at = observed_at,
            .disconnected_at = std::nullopt,
            .last_heartbeat_at = std::nullopt,
        });
    connection = connections_by_id_.find(connection_id);
  }

  if (const auto previous = connection_by_agent_id_.find(agent_id);
      previous != connection_by_agent_id_.end() &&
      previous->second != connection->second.connection_id) {
    if (auto old_connection = connections_by_id_.find(previous->second);
        old_connection != connections_by_id_.end()) {
      connections_by_id_.erase(old_connection);
    }
  }

  connection->second.agent_id = agent_id;
  connection->second.disconnected_at = std::nullopt;
  connection_by_agent_id_.insert_or_assign(std::move(agent_id),
                                           connection->second.connection_id);
}

}  // namespace zfleet::server
