#include "json_codec.h"

#include <nlohmann/json.hpp>

#include <string>

namespace zfleet::installer {

std::string SerializeStatusResult(const StatusResult& result) {
  nlohmann::json payload = {
      {"component", result.component},
      {"state", result.state},
  };
  if (result.version.has_value()) {
    payload["version"] = *result.version;
  }
  if (result.message.has_value()) {
    payload["message"] = *result.message;
  }
  return payload.dump();
}

} // namespace zfleet::installer
