#pragma once

#include "database.h"

#include "zfleet/protocol/v1/agent_control.grpc.pb.h"

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace zfleet::server {

struct ControlConnectionInfo {
  std::string agent_id;
  std::string connected_at;
  std::string last_heartbeat_at;
  bool connected = false;
};

class GrpcControlService final
    : public zfleet::protocol::v1::AgentControl::Service {
 public:
  explicit GrpcControlService(ServerDatabase* database);

  grpc::Status Connect(
      grpc::ServerContext* context,
      grpc::ServerReaderWriter<zfleet::protocol::v1::ServerCommand,
                               zfleet::protocol::v1::AgentEvent>* stream)
      override;

  std::optional<ControlConnectionInfo> FindConnection(
      const std::string& agent_id) const;
  bool IsAgentOnline(const std::string& agent_id,
                     std::chrono::seconds heartbeat_timeout) const;

 private:
  struct StoredConnection {
    ControlConnectionInfo info;
    std::chrono::steady_clock::time_point last_heartbeat_monotonic;
  };

  grpc::Status HandleRegister(const zfleet::protocol::v1::AgentEvent& event,
                              std::string* active_agent_id);
  grpc::Status HandleHeartbeat(const zfleet::protocol::v1::AgentEvent& event);
  void MarkDisconnected(const std::string& agent_id);

  ServerDatabase* database_;
  mutable std::mutex connections_mutex_;
  std::unordered_map<std::string, StoredConnection> connections_;
};

} // namespace zfleet::server
