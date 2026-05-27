#pragma once

#include <optional>
#include <string>

namespace zfleet::server {

struct UpgradeRequestBody {
  std::string package_id;
  std::optional<std::string> set_by;
  std::string expires_at = "2099-12-31T23:59:59Z";
};

struct RollbackRequestBody {
  std::optional<std::string> set_by;
  std::string expires_at = "2099-12-31T23:59:59Z";
};

struct PublishRequestBody {
  std::string channel;
  std::optional<std::string> published_by;
};

struct TokenCreateRequestBody {
  std::string purpose = "agent_register";
  std::optional<std::string> channel;
  std::optional<std::string> platform;
  std::optional<std::string> arch;
  std::optional<int> max_uses = 1;
  std::string expires_at;
};

UpgradeRequestBody ParseUpgradeRequestBody(const std::string& body);
RollbackRequestBody ParseRollbackRequestBody(const std::string& body);
PublishRequestBody ParsePublishRequestBody(const std::string& body);
TokenCreateRequestBody ParseTokenCreateRequestBody(const std::string& body);

}  // namespace zfleet::server
