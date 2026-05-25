#include "management_json_codec.h"

#include <nlohmann/json.hpp>

namespace zfleet::server {
namespace {

nlohmann::json ParseJsonObject(const std::string& body) {
  if (body.empty()) {
    return nlohmann::json::object();
  }
  return nlohmann::json::parse(body);
}

}  // namespace

UpgradeRequestBody ParseUpgradeRequestBody(const std::string& body) {
  const auto json = ParseJsonObject(body);
  UpgradeRequestBody value{
      .package_id = json.value("package_id", std::string{}),
      .set_by = json.contains("set_by")
                    ? std::optional<std::string>{json["set_by"].get<std::string>()}
                    : std::nullopt,
      .expires_at =
          json.value("expires_at", std::string("2099-12-31T23:59:59Z")),
  };
  return value;
}

RollbackRequestBody ParseRollbackRequestBody(const std::string& body) {
  const auto json = ParseJsonObject(body);
  return RollbackRequestBody{
      .set_by = json.contains("set_by")
                    ? std::optional<std::string>{json["set_by"].get<std::string>()}
                    : std::nullopt,
      .expires_at =
          json.value("expires_at", std::string("2099-12-31T23:59:59Z")),
  };
}

PublishRequestBody ParsePublishRequestBody(const std::string& body) {
  const auto json = ParseJsonObject(body);
  return PublishRequestBody{
      .channel = json.value("channel", std::string{}),
      .published_by =
          json.contains("published_by")
              ? std::optional<std::string>{json["published_by"].get<std::string>()}
              : std::nullopt,
  };
}

TokenCreateRequestBody ParseTokenCreateRequestBody(const std::string& body) {
  const auto json = ParseJsonObject(body);
  return TokenCreateRequestBody{
      .purpose = json.value("purpose", "agent_register"),
      .channel = json.contains("channel")
                     ? std::optional<std::string>{json["channel"].get<std::string>()}
                     : std::nullopt,
      .platform =
          json.contains("platform")
              ? std::optional<std::string>{json["platform"].get<std::string>()}
              : std::nullopt,
      .arch = json.contains("arch")
                  ? std::optional<std::string>{json["arch"].get<std::string>()}
                  : std::nullopt,
      .max_uses = json.contains("max_uses")
                      ? std::optional<int>{json["max_uses"].get<int>()}
                      : std::optional<int>{1},
      .expires_at = json.value("expires_at", std::string{}),
  };
}

}  // namespace zfleet::server
