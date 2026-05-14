#pragma once

#include "zfleet/protocol/message.h"

#include <filesystem>
#include <string>

namespace zfleet::server {

class ServerDatabase {
 public:
  explicit ServerDatabase(std::filesystem::path database_path);

  void Initialize();
  int schema_version() const;
  const std::filesystem::path& database_path() const noexcept;
  bool AgentExists(const std::string& agent_id) const;
  void UpsertAgent(const zfleet::protocol::RegistrationRequest& request);
  void RecordHeartbeat(const zfleet::protocol::HeartbeatRequest& request,
                       const std::string& payload_json);
  void RecordAssetSnapshot(
      const zfleet::protocol::AssetSnapshotRequest& request,
      const std::string& payload_json);

 private:
  std::filesystem::path database_path_;
};

} // namespace zfleet::server
